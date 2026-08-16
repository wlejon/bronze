#pragma once

#include <string>
#include <vector>

#include "modules/resolve.h"

namespace bronze::cli {

int runTypes(const std::string& sourcePath, std::string* outString = nullptr,
             const std::vector<modules::ModuleRoot>& moduleRoots = {});

// `infer == false` skips inference entirely and lowers with the uniform dynamic
// convention everywhere, reproducing the pre-inference calling convention
// exactly (lower.h says what it does NOT reproduce). It is the bisection seam
// for a miscompile inference is suspected of causing, and it is what
// `--no-infer` selects.
//
// `hostGlobalsPath` is the `--host-globals` manifest (one identifier per line,
// `#` comments): names an embedding host will register with the runtime, each
// admitted onto lowering's provided-globals set. Empty means no manifest. It
// travels as the PATH rather than a parsed list so the two commands that take
// it report an unreadable file or a bad line through the same error path as
// everything else.
int runIl(const std::string& sourcePath, std::string* outString = nullptr, bool infer = true,
          const std::string& hostGlobalsPath = {},
          const std::vector<modules::ModuleRoot>& moduleRoots = {});
// `timings` prints per-phase wall time to stderr. It defaults off and no test
// passes it: a duration is the one thing bronze emits that cannot be
// deterministic, so it stays out of every path an expectation can see.
//
// `emitObj` stops the pipeline after object emission — `outputPath` receives
// the object file, exactly as given, and no linker runs. It is the embedding
// seam: a host build links the object against bronze's runtime and its own
// code, so bronze must not insist on producing an executable.
//
// `entrySymbol` names the object's exported entry point. Empty means the
// default `bronze_main`, which is what src/rt/rt.cpp and embed's runMain call.
// It exists so a host can link MORE THAN ONE compiled module into one image:
// the entry and the ABI stamp are the only two names an object exports, so
// distinct entry symbols are what keep two modules from colliding at link.
int runBuild(const std::string& sourcePath, const std::string& outputPath,
             std::string* errOut = nullptr, bool infer = true, bool timings = false,
             bool emitObj = false, const std::string& hostGlobalsPath = {},
             bool inferStats = false, std::string* statsOut = nullptr,
             const std::vector<modules::ModuleRoot>& moduleRoots = {},
             const std::string& entrySymbol = {});
int runDriver(int argc, char** argv);

}  // namespace bronze::cli
