#pragma once

#include "il_translator.h"
#include <brass/mir/pass_pipeline.hpp>

namespace il2mir {

// The optimization pipeline configuration a translation with `options` runs.
PassPipelineOptions pass_pipeline_options(const TranslatorOptions& options);

} // namespace il2mir
