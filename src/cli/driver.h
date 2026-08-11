#pragma once

#include <string>

namespace bronze::cli {

int runTypes(const std::string& sourcePath, std::string* outString = nullptr);

// `infer == false` skips inference entirely and lowers with the uniform
// dynamic convention everywhere, reproducing the pre-inference calling
// convention exactly (docs/0010 decision 8; lower.h says what it does NOT
// reproduce). It is the bisection seam for a miscompile inference is
// suspected of causing, and it is what `--no-infer` selects.
int runIl(const std::string& sourcePath, std::string* outString = nullptr, bool infer = true);
int runBuild(const std::string& sourcePath, const std::string& outputPath,
             std::string* errOut = nullptr, bool infer = true);
int runDriver(int argc, char** argv);

}  // namespace bronze::cli
