#pragma once

// The AOT whole-program optimizer's entry point (src/opt/wpo). It runs on the
// final IL of a `bronze build`, after lowering and before the backend; the JIT
// never runs it.
//
// It solves the module twice. The PROVING run (chunk 1's analysis) is what
// holds for every execution; the PREDICTING run (Solver `predict`) is what a
// guarded fast path may assume, and it is the one the build consumes today:
// static-slot claims on property sites lowering left without one (stamp.h).
// `--wpo-stats` adds the census of both runs and of the claims.

#include <string>
#include <vector>

#include "il/il.h"

namespace bronze::wpo {

struct BuildOptions {
    // The host's global names (`--host-globals` and the native manifest's
    // roots): the open world.
    std::vector<std::string> hostGlobals;
    // `--wpo-stats`: return the census text.
    bool stats = false;
    // `--wpo-stats=<substrings>`: also dump every site of the functions whose
    // name contains one of the comma-separated substrings.
    bool perSite = false;
    std::string filter;
};

// Runs the analysis over `mod` and applies its claims. Returns the census
// text when `opts.stats`, else an empty string. What it does to `mod` does
// not depend on `stats`.
std::string optimizeForBuild(il::Module& mod, const BuildOptions& opts);

}  // namespace bronze::wpo
