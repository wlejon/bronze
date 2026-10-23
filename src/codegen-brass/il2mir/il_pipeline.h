#pragma once

#include "il_translator.h"
#include <brass/mir/pass_pipeline.hpp>

namespace il2mir {

// The optimization pipeline configuration a translation with `options` runs.
PassPipelineOptions pass_pipeline_options(const TranslatorOptions& options);

// Builds the full Bronze MIR optimization pipeline (including SMT guard
// elimination and solver-verified peephole superoptimization before WBE).
brass::Pipeline bronze_optimization_pipeline(const TranslatorOptions& options);

// Runs bronze_optimization_pipeline(options) on `mod`.
bool run_bronze_optimization_pipeline(
    brass::Module& mod,
    const TranslatorOptions& options,
    const brass::PassPipelineHooks& hooks = {});

// Runs SMT guard elimination and MIR superoptimization on `mod` (e.g. for
// Tier-2 per-function/OSR copies after speculation guards are armed).
bool run_smt_optimizations(
    brass::Module& mod,
    const TranslatorOptions& options = {});

} // namespace il2mir
