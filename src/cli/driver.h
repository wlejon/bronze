#pragma once

#include <string>

namespace bronze::cli {

int runIl(const std::string& sourcePath, std::string* outString = nullptr);
int runBuild(const std::string& sourcePath, const std::string& outputPath, std::string* errOut = nullptr);
int runDriver(int argc, char** argv);

}  // namespace bronze::cli
