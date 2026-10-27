#include "codegen-brass/brass_symbol_registration.h"
#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "runtime/fn.h"
#include "runtime/stack_locals.h"
#include "runtime/value.h"

#include <brass/gc/heap.hpp>
#include <brass/runtime/code_installer.hpp>
#include <brass/runtime/deterministic_math.hpp>
#include <brass/runtime/tiering.hpp>
#include <brass/gc/runtime_gc.hpp>
#include <brass/interpreter/interpreter.hpp>
#include <brass/runtime/host_symbols.hpp>
#include <brass/runtime/native_function.hpp>
#include <brass/runtime/parallel_runtime.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <type_traits>
#include <vector>
#include <string>
#include <utility>

namespace bronze {

namespace {

using brass::runtime::host_function;

using UnaryMath = double (*)(double);
struct MathSymbol {
    const char* name;
    UnaryMath fn;
};
const MathSymbol kMathSymbols[] = {
    {"sin", &brass::math::det_sin},
    {"cos", &brass::math::det_cos},
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
        interp.register_external_function(#name, host_function<brass::Interpreter>(&::name));
        BRONZE_ABI_FUNCTIONS(BRONZE_ABI_REG_INTERP)
#undef BRONZE_ABI_REG_INTERP
        for (const MathSymbol& m : kMathSymbols) {
            interp.register_external_function(m.name, host_function<brass::Interpreter>(m.fn));
        }
    }

    void install(brass::FastInterpreter& interp) override {
#define BRONZE_ABI_REG_FAST(name, ret, args) \
        interp.register_external_function(#name, brass::FastHostFn(host_function<brass::FastInterpreter>(&::name)));
        BRONZE_ABI_FUNCTIONS(BRONZE_ABI_REG_FAST)
#undef BRONZE_ABI_REG_FAST
        for (const MathSymbol& m : kMathSymbols) {
            interp.register_external_function(m.name, brass::FastHostFn(host_function<brass::FastInterpreter>(m.fn)));
        }
        interp.register_external_symbol("brass_gc_card_table_base", reinterpret_cast<void*>(&brass_gc_card_table_base));
        interp.register_external_symbol("brass_gc_heap_base", reinterpret_cast<void*>(&brass_gc_heap_base));
        // The interrupt poll's handler is called by address at a backward
        // jump (FastInterpreter::run_interrupt_handler), not as a host
        // function.
        interp.register_external_symbol("bronze_interrupt_poll", reinterpret_cast<void*>(&::bronze_interrupt_poll));
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

// brassTieredEnterJsHook's argument vectors, one per nesting depth. Per
// STACK (runtime/stack_locals.h): the nesting is one stack's, and a
// compartment's fiber suspended inside a call keeps its vector.
thread_local std::deque<std::vector<brass::RuntimeValue>> t_enterArgPool;
thread_local size_t t_enterDepth = 0;
std::deque<std::vector<brass::RuntimeValue>>& enterArgPoolRef() { return t_enterArgPool; }
size_t& enterDepthRef() { return t_enterDepth; }
size_t noDepth() { return 0; }
[[maybe_unused]] const int g_enterArgPoolLocal =
    runtime::registerStackLocalBox<std::deque<std::vector<brass::RuntimeValue>>, &enterArgPoolRef>();
[[maybe_unused]] const int g_enterDepthLocal =
    runtime::registerStackLocalWord<size_t, &enterDepthRef, &noDepth>();

bool brassTieredEnterJsHook(bronze_fn_code code, uint64_t env_bits, uint64_t this_bits,
                           uint32_t argc, const uint64_t* argv, uint64_t* out_result) {
    auto* interp = brass::FastInterpreter::current();
    if (!interp) return false;

    uintptr_t codePtr = reinterpret_cast<uintptr_t>(code);
    // What `code` resolves to, per thread, direct-mapped: three hash lookups
    // (two pointer maps, the handle by name under the table's lock) on every
    // call were most of what a closure call from compiled code cost. Valid
    // while the registry generation is the one it was resolved in: a handle
    // made, rebound or retired, and a table destroyed, all move it
    // (code_installer.hpp), which is what the interpreter's own call caches
    // key on.
    struct Resolved {
        uintptr_t code = 0;
        const brass::FastInterpreter* interp = nullptr;
        uint64_t gen = 0;
        const brass::Function* mirFn = nullptr;
        const brass::BytecodeFunction* bfn = nullptr;
        brass::runtime::FunctionHandle* handle = nullptr;
    };
    thread_local Resolved cache[64];
    Resolved& r = cache[(codePtr >> 4) & 63];
    const uint64_t gen = brass::runtime::registry_generation();
    if (r.code != codePtr || r.interp != interp || r.gen != gen) {
        r.code = codePtr;
        r.interp = interp;
        r.gen = gen;
        r.mirFn = interp->find_function_by_pointer(codePtr);
        r.bfn = interp->find_bytecode_function_by_pointer(codePtr);
        r.handle = nullptr;
        if (r.mirFn && codePtr != reinterpret_cast<uintptr_t>(r.mirFn)) {
            brass::runtime::FunctionHandle* h = interp->dispatch_table().find(r.mirFn->name());
            if (h && h->mir_function() == r.mirFn) r.handle = h;
        }
    }
    const brass::Function* mirFn = r.mirFn;
    const brass::BytecodeFunction* bfn = r.bfn;
    if (!mirFn && !bfn) return false;

    // A function that already has native code (tier 1, tier 2) is left to
    // rtEnterJs, which enters `code` through the trampoline every compiled
    // call takes. `code` is the function's module stub (a closure's code
    // pointer in every tier, MultiTierPipeline::function_address), whose
    // cell follows its best code. Running it through the interpreter instead
    // looked its handle up by name, boxed the four arguments into a vector
    // and crossed the generic invoke thunk on every call: a closure called
    // from optimized or OSR code (a `const rnd = () => ...` in a hot loop)
    // paid that per iteration, 3x the cost of the same loop compiled whole.
    // A pointer that is the Function itself (registered for a module the
    // interpreter was handed whole) is not code, and still runs here.
    if (r.handle && r.handle->native_entry()) return false;

    // One argument vector per nesting depth, reused: every JS call the
    // interpreter takes from native code comes through here, and a fresh
    // vector per call was a heap allocation each. A deque, so growing it
    // leaves the outer calls' vectors where they are.
    std::deque<std::vector<brass::RuntimeValue>>& argPool = t_enterArgPool;
    size_t& depth = t_enterDepth;
    if (argPool.size() <= depth) argPool.resize(depth + 1);
    std::vector<brass::RuntimeValue>& wrapArgs = argPool[depth];
    wrapArgs.clear();
    wrapArgs.push_back(brass::RuntimeValue::from_i64(static_cast<int64_t>(env_bits)));
    wrapArgs.push_back(brass::RuntimeValue::from_i64(static_cast<int64_t>(this_bits)));
    wrapArgs.push_back(brass::RuntimeValue::from_i32(static_cast<int32_t>(argc)));
    wrapArgs.push_back(brass::RuntimeValue::from_ptr(reinterpret_cast<uintptr_t>(argv)));
    struct DepthScope {
        size_t& d;
        explicit DepthScope(size_t& x) : d(x) { ++d; }
        ~DepthScope() { --d; }
    } scope(depth);
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
