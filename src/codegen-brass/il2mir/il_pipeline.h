#pragma once

#include "il_translator.h"
#include <brass/mir/pass_pipeline.hpp>

namespace il2mir {

// The optimization pipeline configuration a translation with `options` runs.
PassPipelineOptions pass_pipeline_options(const TranslatorOptions& options);

// Builds the full Bronze MIR optimization pipeline: brass's pass pipeline for
// `options`, with write-barrier elimination last.
brass::Pipeline bronze_optimization_pipeline(const TranslatorOptions& options);

// Runs bronze_optimization_pipeline(options) on `mod`.
bool run_bronze_optimization_pipeline(
    brass::Module& mod,
    const TranslatorOptions& options,
    const brass::PassPipelineHooks& hooks = {});

} // namespace il2mir
