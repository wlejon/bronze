#pragma once

#include <string>
#include <vector>

namespace bronze::host {

struct CliHostConfig {
    std::vector<std::string> args;
    std::string exePath;
    void (*beforeExit)() = nullptr;
};

// Canonical path to the currently running executable.
std::string ownExecutablePath();

// Registers the lazy `Bronze` CLI/OS host global in the current realm.
// Zero allocation until a script actually references `Bronze`.
// Installed only by `bronze_host` (standalone built executables) and `bronze`
// CLI (`run` / `eval`), never by `bronze_runtime` / `bronze_embed`.
void installCliHost(CliHostConfig cfg);

}  // namespace bronze::host
