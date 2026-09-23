#include "codegen-brass/brass_symbol_registration.h"
#include "codegen-brass/brass_coroutine_bridge.h"
#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "runtime/fn.h"
#include "runtime/value.h"

#include <brass/il_translator/il_translator.hpp>
#include <brass/runtime/parallel_runtime.hpp>
#include <cmath>
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

template <typename Ret, typename... Args>
brass::FastHostFn wrapAbiFunction(Ret (*fn)(Args...)) {
    return [fn](brass::FastInterpreter&, const std::vector<brass::RuntimeValue>& args) -> brass::RuntimeValue {
        return invokeHelper(fn, args, std::index_sequence_for<Args...>{});
    };
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

void registerBronzeFastInterpreterSymbols(brass::FastInterpreter& interp) {
    // Install the trampoline interceptor for interpreted Bronze functions.
    // Through embed, not rtSetEnterJsHook: the hook must land in the process's
    // one runtime (embed.h, setEnterJsHook).
    embed::setEnterJsHook(&brassTieredEnterJsHook);

    // 1. Built-in registration from Brass il translator
    brass::il::register_bronze_fast_interpreter_symbols(&interp);

    // 2. Register all Bronze ABI functions with universal typed wrappers
#define BRONZE_ABI_REG_FAST(name, ret, args) \
    interp.register_external_function(#name, wrapAbiFunction(&::name));
    BRONZE_ABI_FUNCTIONS(BRONZE_ABI_REG_FAST)
#undef BRONZE_ABI_REG_FAST

    // 3. Register standard math functions
    interp.register_external_function("sin", wrapAbiFunction(static_cast<double(*)(double)>(&std::sin)));
    interp.register_external_function("cos", wrapAbiFunction(static_cast<double(*)(double)>(&std::cos)));
    interp.register_external_function("sqrt", wrapAbiFunction(static_cast<double(*)(double)>(&std::sqrt)));
    interp.register_external_function("fabs", wrapAbiFunction(static_cast<double(*)(double)>(&std::fabs)));
    interp.register_external_function("floor", wrapAbiFunction(static_cast<double(*)(double)>(&std::floor)));
    interp.register_external_function("ceil", wrapAbiFunction(static_cast<double(*)(double)>(&std::ceil)));
    interp.register_external_function("trunc", wrapAbiFunction(static_cast<double(*)(double)>(&std::trunc)));

    // 4. Register default fallback data symbols
    interp.register_external_symbol("__bronze_module_env", &s_default_module_env);
    interp.register_external_symbol("__bronze_key_map", s_default_key_map);
    interp.register_external_symbol("__bronze_template_cells", s_default_template_cells);
    interp.register_external_symbol("__bronze_global_cache", s_default_global_cache);
    interp.register_external_symbol("__bronze_ic_table", s_default_ic_table);
    interp.register_external_symbol("__bronze_method_ic_sites", s_default_method_ic_sites);
    interp.register_external_symbol("bronze_main_key_constants", s_default_key_manifest);
    interp.register_external_symbol("main_key_constants", s_default_key_manifest);

    // 5. Register Brass coroutine runtime symbols
    registerBrassCoroutineSymbols(interp);
}

void registerBronzeBaselineSymbols(brass::codegen::BaselineJitCompiler& compiler) {
    // 1. Register symbols via Brass il translator
    brass::il::register_bronze_baseline_symbols(&compiler);

    // 2. Register all ABI functions directly as native pointers
#define BRONZE_ABI_REG_BASELINE(name, ret, args) \
    compiler.register_external_symbol(#name, reinterpret_cast<void*>(&::name));
    BRONZE_ABI_FUNCTIONS(BRONZE_ABI_REG_BASELINE)
#undef BRONZE_ABI_REG_BASELINE

    // 3. Register standard math functions
    compiler.register_external_symbol("sin", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::sin)));
    compiler.register_external_symbol("cos", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::cos)));
    compiler.register_external_symbol("sqrt", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::sqrt)));
    compiler.register_external_symbol("fabs", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::fabs)));
    compiler.register_external_symbol("floor", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::floor)));
    compiler.register_external_symbol("ceil", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::ceil)));
    compiler.register_external_symbol("trunc", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::trunc)));

    // 3b. Register ParallelRuntime symbols
    compiler.register_external_symbol("brass_parallel_for_chunks", reinterpret_cast<void*>(&brass_parallel_for));
    compiler.register_external_symbol("brass_parallel_for", reinterpret_cast<void*>(&brass_parallel_for));
    compiler.register_external_symbol("brass_set_parallel_workers", reinterpret_cast<void*>(&brass_set_parallel_workers));
    compiler.register_external_symbol("brass_get_parallel_workers", reinterpret_cast<void*>(&brass_get_parallel_workers));
    compiler.register_external_symbol("brass_parallel_reduce_i64", reinterpret_cast<void*>(&brass_parallel_reduce_i64));
    compiler.register_external_symbol("brass_parallel_reduce_f64", reinterpret_cast<void*>(&brass_parallel_reduce_f64));
    compiler.register_external_symbol("brass_parallel_alloc_context", reinterpret_cast<void*>(&brass_parallel_alloc_context));
    compiler.register_external_symbol("brass_parallel_free_context", reinterpret_cast<void*>(&brass_parallel_free_context));

    // 4. Register default fallback data symbols
    compiler.register_external_symbol("__bronze_module_env", &s_default_module_env);
    compiler.register_external_symbol("__bronze_key_map", s_default_key_map);
    compiler.register_external_symbol("__bronze_template_cells", s_default_template_cells);
    compiler.register_external_symbol("__bronze_global_cache", s_default_global_cache);
    compiler.register_external_symbol("__bronze_ic_table", s_default_ic_table);
    compiler.register_external_symbol("__bronze_method_ic_sites", s_default_method_ic_sites);
    compiler.register_external_symbol("bronze_main_key_constants", s_default_key_manifest);
    compiler.register_external_symbol("main_key_constants", s_default_key_manifest);

    // 5. Register Brass coroutine runtime symbols
    registerBrassCoroutineSymbols(compiler);
}

void registerBronzeMultiTierSymbols(brass::runtime::MultiTierPipeline& pipeline) {
    embed::setEnterJsHook(&brassTieredEnterJsHook);
    registerBronzeBaselineSymbols(pipeline.baseline_compiler());

#define BRONZE_ABI_REG_PIPELINE(name, ret, args) \
    pipeline.register_external_symbol(#name, reinterpret_cast<void*>(&::name)); \
    pipeline.register_external_function(#name, wrapAbiFunction(&::name));
    BRONZE_ABI_FUNCTIONS(BRONZE_ABI_REG_PIPELINE)
#undef BRONZE_ABI_REG_PIPELINE

    pipeline.register_external_symbol("sin", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::sin)));
    pipeline.register_external_function("sin", wrapAbiFunction(static_cast<double(*)(double)>(&std::sin)));
    pipeline.register_external_symbol("cos", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::cos)));
    pipeline.register_external_function("cos", wrapAbiFunction(static_cast<double(*)(double)>(&std::cos)));
    pipeline.register_external_symbol("sqrt", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::sqrt)));
    pipeline.register_external_function("sqrt", wrapAbiFunction(static_cast<double(*)(double)>(&std::sqrt)));
    pipeline.register_external_symbol("fabs", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::fabs)));
    pipeline.register_external_function("fabs", wrapAbiFunction(static_cast<double(*)(double)>(&std::fabs)));
    pipeline.register_external_function("floor", wrapAbiFunction(static_cast<double(*)(double)>(&std::floor)));
    pipeline.register_external_symbol("floor", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::floor)));
    pipeline.register_external_symbol("ceil", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::ceil)));
    pipeline.register_external_function("ceil", wrapAbiFunction(static_cast<double(*)(double)>(&std::ceil)));
    pipeline.register_external_symbol("trunc", reinterpret_cast<void*>(static_cast<double(*)(double)>(&std::trunc)));
    pipeline.register_external_function("trunc", wrapAbiFunction(static_cast<double(*)(double)>(&std::trunc)));

    // 3b. Register ParallelRuntime symbols
    pipeline.register_external_symbol("brass_parallel_for_chunks", reinterpret_cast<void*>(&brass_parallel_for));
    pipeline.register_external_symbol("brass_parallel_for", reinterpret_cast<void*>(&brass_parallel_for));
    pipeline.register_external_symbol("brass_set_parallel_workers", reinterpret_cast<void*>(&brass_set_parallel_workers));
    pipeline.register_external_symbol("brass_get_parallel_workers", reinterpret_cast<void*>(&brass_get_parallel_workers));
    pipeline.register_external_symbol("brass_parallel_reduce_i64", reinterpret_cast<void*>(&brass_parallel_reduce_i64));
    pipeline.register_external_symbol("brass_parallel_reduce_f64", reinterpret_cast<void*>(&brass_parallel_reduce_f64));
    pipeline.register_external_symbol("brass_parallel_alloc_context", reinterpret_cast<void*>(&brass_parallel_alloc_context));
    pipeline.register_external_symbol("brass_parallel_free_context", reinterpret_cast<void*>(&brass_parallel_free_context));

    pipeline.register_external_symbol("__bronze_module_env", &s_default_module_env);
    pipeline.register_external_symbol("__bronze_key_map", s_default_key_map);
    pipeline.register_external_symbol("__bronze_template_cells", s_default_template_cells);
    pipeline.register_external_symbol("__bronze_global_cache", s_default_global_cache);
    pipeline.register_external_symbol("__bronze_ic_table", s_default_ic_table);
    pipeline.register_external_symbol("__bronze_method_ic_sites", s_default_method_ic_sites);
    pipeline.register_external_symbol("bronze_main_key_constants", s_default_key_manifest);
    pipeline.register_external_symbol("main_key_constants", s_default_key_manifest);

    // 5. Register Brass coroutine runtime symbols
    registerBrassCoroutineSymbols(pipeline);
}

}  // namespace bronze
