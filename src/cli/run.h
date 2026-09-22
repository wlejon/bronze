#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codegen-brass/brass_tiered_engine.h"

namespace bronze::cli {

using RunEvalFn = int (*)(std::string_view, std::optional<ExecutionTier>, bool);
// The file to run, the names a `--host-globals` manifest listed (empty
// when none was given, in which case the JIT reads the host's registry),
// an optional execution tier, and whether to emit native debug information.
using RunFileInJitFn = int (*)(const std::string&, const std::vector<std::string>&,
                              std::optional<ExecutionTier>, bool);

void registerRunHooks(RunEvalFn evalFn, RunFileInJitFn fileFn);
int runEvalReal(std::string_view code, std::optional<ExecutionTier> tier = std::nullopt, bool emitDebugInfo = false);
int runFileInJitReal(const std::string& filePath, const std::vector<std::string>& hostGlobals,
                     std::optional<ExecutionTier> tier = std::nullopt, bool emitDebugInfo = false);

}  // namespace bronze::cli
