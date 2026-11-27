#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codegen-brass/brass_tiered_engine.h"

namespace bronze::cli {

using RunEvalFn = int (*)(std::string_view, bool);
// The file to run, the names a `--host-globals` manifest listed (empty
// when none was given, in which case the JIT reads the host's registry),
// whether to emit native debug information, and whether the file is a classic
// script (`--classic[=<marker>]`: the EvalOptions::classicScriptMarker, empty
// for one script).
using RunFileInJitFn = int (*)(const std::string&, const std::vector<std::string>&,
                              bool, const std::optional<std::string>&,
                              const std::vector<std::string>&);

void registerRunHooks(RunEvalFn evalFn, RunFileInJitFn fileFn);
int runEvalReal(std::string_view code, bool emitDebugInfo = false);
int runFileInJitReal(const std::string& filePath, const std::vector<std::string>& hostGlobals,
                     bool emitDebugInfo, const std::optional<std::string>& classic,
                     const std::vector<std::string>& scriptArgs = {});

}  // namespace bronze::cli
