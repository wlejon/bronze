#include "codegen-brass/brass_symbol_registration.h"
#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "runtime/fn.h"
#include "runtime/value.h"

#include <brass/gc/heap.hpp>
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

class BronzeHostSymbols final : public brass::runtime::HostSymbolProvider {
public:
    void install(brass::codegen::JitExecutionEngine& engine) override {
        registerNativeSymbols(engine);
    }

    void install(brass::codegen::BaselineJitCompiler& compiler) override {
        registerNativeSymbols(compiler);
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

// Bronze's objects live on the thread's bronze heap, which is a brass
// gc::Heap bound as the thread's current heap (runtime/heap.h, bind_thread).
// Every brass interpreter built on a thread after that uses it: its frames
// are that heap's roots and its checked memory accesses know its objects.
// installBronzeHostSymbols creates the calling thread's heap first for that
// reason. An interpreter built on a thread that has none makes a private heap
// of its own; nothing bronze compiles allocates through brass's runtime, so
// such a heap holds nothing, and this configuration keeps it the smallest a
// heap can be.
brass::gc::HeapConfig privateHeapConfig() {
    brass::gc::HeapConfig config;
    config.eden_bytes = 0;
    config.survivor_bytes = 0;
    config.mature_reserve_bytes = 0;
    config.large_reserve_bytes = 0;
    config.read_environment = false;
    return config;
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
    brass::gc::Heap::set_default_config(privateHeapConfig());
    embed::bindThreadHeap();
}

void installBronzeEnterJsHook() {
    // Through embed, not rtSetEnterJsHook: the hook must land in the process's
    // one runtime (embed.h, setEnterJsHook).
    embed::setEnterJsHook(&brassTieredEnterJsHook);
}

void registerBronzeJitSymbols(brass::codegen::JitExecutionEngine& engine) {
    installBronzeHostSymbols();
    hostSymbols().install(engine);
}

void registerBronzeMultiTierSymbols(brass::runtime::MultiTierPipeline& pipeline) {
    installBronzeHostSymbols();
    hostSymbols().install(pipeline.baseline_compiler());
}

}  // namespace bronze
