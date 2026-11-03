#pragma once

// The AOT whole-program optimizer's entry points (src/opt/wpo). It runs on the
// final IL of a `bronze build`, after lowering and before the backend, and
// only when a consumer asks for it: today the `--wpo-stats` census, which
// reads the module and changes nothing.

#include <string>
#include <vector>

#include "il/il.h"

namespace bronze::wpo {

struct StatsOptions {
    // The host's global names (`--host-globals` and the native manifest's
    // roots): the open world.
    std::vector<std::string> hostGlobals;
    // `--wpo-stats=<substrings>`: also dump every site of the functions whose
    // name contains one of the comma-separated substrings.
    bool perSite = false;
    std::string filter;
};

// Runs the analysis over `mod` and returns the census text.
std::string statsReport(const il::Module& mod, const StatsOptions& opts);

}  // namespace bronze::wpo
