#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace bronze::cli {

using RunEvalFn = int (*)(std::string_view);
// The file to run and the names a `--host-globals` manifest listed (empty
// when none was given, in which case the JIT reads the host's registry).
using RunFileInJitFn = int (*)(const std::string&, const std::vector<std::string>&);

void registerRunHooks(RunEvalFn evalFn, RunFileInJitFn fileFn);
int runEvalReal(std::string_view code);
int runFileInJitReal(const std::string& filePath, const std::vector<std::string>& hostGlobals);

}  // namespace bronze::cli
