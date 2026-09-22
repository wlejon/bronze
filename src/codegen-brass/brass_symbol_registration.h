#pragma once

#include <brass/vm/fast_interpreter.hpp>
#include <brass/codegen/baseline_jit.hpp>
#include <brass/runtime/multi_tier_pipeline.hpp>

namespace bronze {

// Registers all Bronze ABI helper symbols, functions, math routines, and fallback
// data structures into the given FastInterpreter instance.
void registerBronzeFastInterpreterSymbols(brass::FastInterpreter& interp);

// Registers all Bronze ABI helper symbols and math routines into the given
// BaselineJitCompiler instance.
void registerBronzeBaselineSymbols(brass::codegen::BaselineJitCompiler& compiler);

// Registers all Bronze ABI helper symbols into the MultiTierPipeline instance.
void registerBronzeMultiTierSymbols(brass::runtime::MultiTierPipeline& pipeline);

}  // namespace bronze
