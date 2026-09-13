#pragma once

#include <string>
#include <string_view>

namespace bronze::cli {

using RunEvalFn = int (*)(std::string_view);
using RunFileInJitFn = int (*)(const std::string&);

void registerRunHooks(RunEvalFn evalFn, RunFileInJitFn fileFn);
int runEvalReal(std::string_view code);
int runFileInJitReal(const std::string& filePath);

}  // namespace bronze::cli
