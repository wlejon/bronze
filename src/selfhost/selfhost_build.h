#pragma once

#include <string>

namespace bronze::selfhost {

// Compiles the self-hosted builtins' library (src/selfhost, the text
// selfhost_library.h carries) into the object `outputPath`, entry
// `bronze_selfhost_main`. Diagnostics go to stderr; the answer is a process
// exit code.
int runSelfHostBuild(const std::string& outputPath);

}  // namespace bronze::selfhost
