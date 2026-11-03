#pragma once

// The AOT whole-program optimizer's entry points (src/opt/wpo). Lowering runs
// both when a `bronze build` asks it to (lower.h `WholeProgramOptions`): the
// direct targets before it copies bodies, the claims on its final IL. The JIT
// never runs them.
//
// The analysis has two modes. A PROVING solve is what holds for every
// execution; a PREDICTING solve (Solver `predict`) is what a guarded fast path
// may assume, and it is the one the build consumes: direct method targets,
// static-slot claims and polymorphic marks on the sites lowering left without
// them (stamp.h). `--wpo-stats` adds the census of both modes and of the
// claims.

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

// The half that runs inside lowering, before the passes that copy callee
// bodies (guarded inlining, typed entries): a predicting solve whose direct
// method targets it applies, so those passes see them.
// Returns its report line when `opts.stats`.
std::string predictDirectTargets(il::Module& mod, const BuildOptions& opts);

// Runs the analysis over `mod` and applies its claims. Returns the census
// text when `opts.stats`, else an empty string. What it does to `mod` does
// not depend on `stats`.
std::string optimizeForBuild(il::Module& mod, const BuildOptions& opts);

}  // namespace bronze::wpo
