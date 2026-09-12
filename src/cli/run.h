#pragma once

#include <string>
#include <string_view>

namespace bronze::cli {

int runEval(std::string_view code);
int runFileInJit(const std::string& filePath);

}  // namespace bronze::cli
