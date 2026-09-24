#include "codegen-brass/brass_symbol_registration.h"
#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "runtime/fn.h"
#include "runtime/value.h"

#include <brass/gc/host_heap.hpp>
#include <brass/gc/runtime_gc.hpp>
#include <brass/interpreter/interpreter.hpp>
#include <brass/runtime/host_symbols.hpp>
#include <brass/runtime/parallel_runtime.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <vector>
#include <string>
#include <utility>

namespace bronze {

namespace {

// Default static fallback buffers for module data sections
static uint64_t s_default_module_env = BRONZE_ABI_UNDEFINED_BITS;
static uint32_t s_default_key_map[4096] = {0};
static uint64_t s_default_template_cells[1024] = {0};
static uint64_t s_default_global_cache[1024] = {0};
static uint8_t s_default_ic_table[4096 * BRONZE_ABI_IC_SITE_SIZE] = {0};
static uint64_t s_default_method_ic_sites[512] = {0};
static uint8_t s_default_key_manifest[256] = {0};

template <typename T>
T unpackArg(const std::vector<brass::RuntimeValue>& args, size_t i) {
    if (i >= args.size()) return T{};
    if constexpr (std::is_same_v<T, double>) {
        return args[i].as_f64();
    } else if constexpr (std::is_same_v<T, float>) {
        return args[i].as_f32();
    } else if constexpr (std::is_same_v<T, int32_t>) {
        return args[i].as_i32();
    } else if constexpr (std::is_same_v<T, uint32_t>) {
        return static_cast<uint32_t>(args[i].as_u32());
    } else if constexpr (std::is_same_v<T, bool>) {
        return args[i].as_i32() != 0;
    } else if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(args[i].as_ptr());
    } else {
        return static_cast<T>(args[i].as_u64());
    }
}

template <typename Ret>
brass::RuntimeValue packResult(Ret val) {
    if constexpr (std::is_void_v<Ret>) {
        return brass::RuntimeValue::from_void();
    } else if constexpr (std::is_same_v<Ret, double>) {
        return brass::RuntimeValue::from_f64(val);
    } else if constexpr (std::is_same_v<Ret, float>) {
        return brass::RuntimeValue::from_f32(val);
    } else if constexpr (std::is_same_v<Ret, int32_t>) {
        return brass::RuntimeValue::from_i32(val);
    } else if constexpr (std::is_same_v<Ret, uint32_t>) {
        return brass::RuntimeValue::from_i32(static_cast<int32_t>(val));
    } else if constexpr (std::is_same_v<Ret, bool>) {
        return brass::RuntimeValue::from_i32(val ? 1 : 0);
    } else if constexpr (std::is_pointer_v<Ret>) {
        return brass::RuntimeValue::from_ptr(reinterpret_cast<uintptr_t>(val));
    } else {
        return brass::RuntimeValue::from_u64(static_cast<uint64_t>(val));
    }
}

template <typename Ret, typename... Args, size_t... Is>
brass::RuntimeValue invokeHelper(Ret (*fn)(Args...), const std::vector<brass::RuntimeValue>& args, std::index_sequence<Is...>) {
    if constexpr (std::is_void_v<Ret>) {
        fn(unpackArg<Args>(args, Is)...);
        return brass::RuntimeValue::from_void();
    } else {
        return packResult(fn(unpackArg<Args>(args, Is)...));
    }
}

// A native helper as an interpreter host function: the fast interpreter's
// (FastHostFn) or the reference interpreter's (brass::HostFn).
template <typename Interp, typename Ret, typename... Args>
auto wrapAbiFunction(Ret (*fn)(Args...)) {
    return [fn](Interp&, const std::vector<brass::RuntimeValue>& args) -> brass::RuntimeValue {
        return invokeHelper(fn, args, std::index_sequence_for<Args...>{});
    };
}

using UnaryMath = double (*)(double);
struct MathSymbol {
    const char* name;
    UnaryMath fn;
};
const MathSymbol kMathSymbols[] = {
    {"sin", static_cast<UnaryMath>(&std::sin)},
    {"cos", static_cast<UnaryMath>(&std::cos)},
    {"sqrt", static_cast<UnaryMath>(&std::sqrt)},
    {"fabs", static_cast<UnaryMath>(&std::fabs)},
    {"floor", static_cast<UnaryMath>(&std::floor)},
    {"ceil", static_cast<UnaryMath>(&std::ceil)},
    {"trunc", static_cast<UnaryMath>(&std::trunc)},
};

// Every symbol a native engine (a JIT or the baseline compiler) links the
// translator's MIR against, by address.
template <typename Engine>
void registerNativeSymbols(Engine& e) {
#define BRONZE_ABI_REG_NATIVE(name, ret, args) \
    e.register_external_symbol(#name, reinterpret_cast<void*>(&::name));
    BRONZE_ABI_FUNCTIONS(BRONZE_ABI_REG_NATIVE)
#undef BRONZE_ABI_REG_NATIVE

    for (const MathSymbol& m : kMathSymbols) e.register_external_symbol(m.name, reinterpret_cast<void*>(m.fn));

    e.register_external_symbol("brass_gc_card_table_base", reinterpret_cast<void*>(&brass_gc_card_table_base));
    e.register_external_symbol("brass_gc_heap_base", reinterpret_cast<void*>(&brass_gc_heap_base));
    e.register_external_symbol("brass_parallel_for_chunks", reinterpret_cast<void*>(&brass_parallel_for));
    e.register_external_symbol("brass_parallel_for", reinterpret_cast<void*>(&brass_parallel_for));
    e.register_external_symbol("brass_set_parallel_workers", reinterpret_cast<void*>(&brass_set_parallel_workers));
    e.register_external_symbol("brass_get_parallel_workers", reinterpret_cast<void*>(&brass_get_parallel_workers));
    e.register_external_symbol("brass_parallel_reduce_i64", reinterpret_cast<void*>(&brass_parallel_reduce_i64));
    e.register_external_symbol("brass_parallel_reduce_f64", reinterpret_cast<void*>(&brass_parallel_reduce_f64));
    e.register_external_symbol("brass_parallel_alloc_context", reinterpret_cast<void*>(&brass_parallel_alloc_context));
    e.register_external_symbol("brass_parallel_free_context", reinterpret_cast<void*>(&brass_parallel_free_context));
}

// The fallback module tables, for a module whose own the engine lacks.
template <typename Engine>
void registerDefaultDataSymbols(Engine& e) {
    e.register_external_symbol("__bronze_module_env", &s_default_module_env);
    e.register_external_symbol("__bronze_key_map", s_default_key_map);
    e.register_external_symbol("__bronze_template_cells", s_default_template_cells);
    e.register_external_symbol("__bronze_global_cache", s_default_global_cache);
    e.register_external_symbol("__bronze_ic_table", s_default_ic_table);
    e.register_external_symbol("__bronze_method_ic_sites", s_default_method_ic_sites);
    e.register_external_symbol("bronze_main_key_constants", s_default_key_manifest);
    e.register_external_symbol("main_key_constants", s_default_key_manifest);
}

class BronzeHostSymbols final : public brass::runtime::HostSymbolProvider {
public:
    void install(brass::codegen::JitExecutionEngine& engine) override {
        registerNativeSymbols(engine);
        registerDefaultDataSymbols(engine);
    }

    void install(brass::codegen::BaselineJitCompiler& compiler) override {
        registerNativeSymbols(compiler);
        registerDefaultDataSymbols(compiler);
    }

    void install(brass::Interpreter& interp) override {
#define BRONZE_ABI_REG_INTERP(name, ret, args) \
        interp.register_external_function(#name, wrapAbiFunction<brass::Interpreter>(&::name));
        BRONZE_ABI_FUNCTIONS(BRONZE_ABI_REG_INTERP)
#undef BRONZE_ABI_REG_INTERP
        for (const MathSymbol& m : kMathSymbols) {
            interp.register_external_function(m.name, wrapAbiFunction<brass::Interpreter>(m.fn));
        }
    }

    void install(brass::FastInterpreter& interp) override {
#define BRONZE_ABI_REG_FAST(name, ret, args) \
        interp.register_external_function(#name, brass::FastHostFn(wrapAbiFunction<brass::FastInterpreter>(&::name)));
        BRONZE_ABI_FUNCTIONS(BRONZE_ABI_REG_FAST)
#undef BRONZE_ABI_REG_FAST
        for (const MathSymbol& m : kMathSymbols) {
            interp.register_external_function(m.name, brass::FastHostFn(wrapAbiFunction<brass::FastInterpreter>(m.fn)));
        }
        interp.register_external_symbol("brass_gc_card_table_base", reinterpret_cast<void*>(&brass_gc_card_table_base));
        interp.register_external_symbol("brass_gc_heap_base", reinterpret_cast<void*>(&brass_gc_heap_base));
        registerDefaultDataSymbols(interp);
    }

    // The block a pinned-TLS read sees before the module entry has loaded the
    // register, and that native code entered from C++ (a tiered-up function
    // the interpreter calls) finds there: the calling thread's.
    void* pinned_tls_block() override { return ::bronze_tls_block_addr(); }
};

BronzeHostSymbols& hostSymbols() {
    static BronzeHostSymbols provider;
    return provider;
}

// Bronze's heap, as brass's runtime sees it. Everything code bronze compiled
// allocates — objects, arrays, environments, closures, the async machines
// and generator state an `.resume` closure carries — is allocated by
// bronze's runtime helpers into bronze's heap and rooted by the function's GC
// frame, whichever tier runs it. What reaches this class is an allocation
// brass's own runtime makes (`brass_gc_alloc` from MIR, a brass coroutine
// frame), which bronze's lowering never emits. It is a hard stop rather than
// a quiet malloc so that no object can ever live outside bronze's collector.
// It aborts itself rather than calling bronze::fatal: this library does not
// link the runtime (see CMakeLists.txt), and the shared runtime a host like
// bro loads does not export fatal.
//
// brass calls allocate_at() and safepoint_at() (never allocate() or
// safepoint() directly), so those are the entry points overridden here.
// Collections are bronze's own, at its allocation points (Heap::collect),
// and every one of them also visits the gcref slots brass knows on the
// thread through brass_enumerate_thread_roots (runtime/rt_state.cpp,
// visitBrassThreadRoots): brass's native-frame scopes, thread-root scopes,
// coroutine frames and interpreter frames are roots of the one heap.
class BronzeHostHeap final : public brass::HostHeap {
public:
    uintptr_t allocate(size_t size, uint64_t pointer_mask, uint32_t type_tag) override {
        return allocate_at(size, pointer_mask, type_tag, 0, 0);
    }

    uintptr_t allocate_at(size_t size, uint64_t /*pointer_mask*/, uint32_t type_tag,
                          uintptr_t caller_fp, uintptr_t caller_ip) override {
        std::fprintf(stderr,
                     "bronze: fatal: brass's runtime asked for a %zu-byte object (type tag %u, "
                     "caller fp 0x%llx ip 0x%llx) while running bronze code; bronze allocates "
                     "only through its own runtime, so a brass heap allocation means compiled "
                     "code reached a brass allocator it must not\n",
                     size, static_cast<unsigned>(type_tag),
                     static_cast<unsigned long long>(caller_fp),
                     static_cast<unsigned long long>(caller_ip));
        std::fflush(stderr);
        std::abort();
    }

    // A safepoint is an opportunity, never a demand, and bronze takes none:
    // its collector runs where bronze's runtime allocates, which is where its
    // GC frames are known to be complete. Overridden explicitly so the
    // choice is visible rather than inherited.
    void safepoint_at(uintptr_t /*caller_fp*/, uintptr_t /*caller_ip*/) override {}
    void safepoint() override {}

    // An interpreter's explicit brass_gc_collect. bronze's lowering never
    // emits one; declined like a safepoint for the same reason.
    void collect() override {}
};

BronzeHostHeap& hostHeap() {
    static BronzeHostHeap heap;
    return heap;
}

bool brassTieredEnterJsHook(bronze_fn_code code, uint64_t env_bits, uint64_t this_bits,
                           uint32_t argc, const uint64_t* argv, uint64_t* out_result) {
    auto* interp = brass::FastInterpreter::current();
    if (!interp) return false;

    uintptr_t codePtr = reinterpret_cast<uintptr_t>(code);
    const brass::Function* mirFn = interp->find_function_by_pointer(codePtr);
    const brass::BytecodeFunction* bfn = interp->find_bytecode_function_by_pointer(codePtr);
    if (!mirFn && !bfn) return false;

    std::vector<brass::RuntimeValue> wrapArgs = {
        brass::RuntimeValue::from_i64(static_cast<int64_t>(env_bits)),
        brass::RuntimeValue::from_i64(static_cast<int64_t>(this_bits)),
        brass::RuntimeValue::from_i32(static_cast<int32_t>(argc)),
        brass::RuntimeValue::from_ptr(reinterpret_cast<uintptr_t>(argv))
    };
    brass::RuntimeValue res = bfn ? interp->run(*bfn, wrapArgs)
                                  : interp->run(*mirFn, wrapArgs);
    *out_result = res.raw_bits();
    return true;
}

}  // namespace

void installBronzeHostSymbols() {
    brass::runtime::set_host_symbol_provider(&hostSymbols());
    brass::set_host_heap(&hostHeap());
}

void registerBronzeFastInterpreterSymbols(brass::FastInterpreter& interp) {
    installBronzeHostSymbols();
    // Install the trampoline interceptor for interpreted Bronze functions.
    // Through embed, not rtSetEnterJsHook: the hook must land in the process's
    // one runtime (embed.h, setEnterJsHook).
    embed::setEnterJsHook(&brassTieredEnterJsHook);
    hostSymbols().install(interp);
}

void registerBronzeBaselineSymbols(brass::codegen::BaselineJitCompiler& compiler) {
    installBronzeHostSymbols();
    hostSymbols().install(compiler);
}

void registerBronzeJitSymbols(brass::codegen::JitExecutionEngine& engine) {
    installBronzeHostSymbols();
    hostSymbols().install(engine);
}

void registerBronzeMultiTierSymbols(brass::runtime::MultiTierPipeline& pipeline) {
    installBronzeHostSymbols();
    embed::setEnterJsHook(&brassTieredEnterJsHook);
    hostSymbols().install(pipeline.baseline_compiler());
}

}  // namespace bronze
