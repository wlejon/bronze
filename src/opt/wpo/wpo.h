#pragma once

// The whole-program analysis's entry point (src/opt/wpo). It computes facts
// about a module and writes nothing into it: what holds for every execution
// (a PROVING solve) or what a guarded fast path may assume (a PREDICTING
// solve, Solver `predict`), the abstract shape of every allocation site, and
// for every property and method site what its receivers are (census.h
// `SiteFact`). Consumers read the result; none of it is applied here.

#include <memory>
#include <string>
#include <vector>

#include "il/il.h"
#include "opt/wpo/census.h"
#include "opt/wpo/shape.h"
#include "opt/wpo/solver.h"

namespace bronze::wpo {

struct AnalysisOptions {
    // The host's global names (`--host-globals` and the native manifest's
    // roots): the open world.
    std::vector<std::string> hostGlobals;
    // A predicting solve (true) or a proving one (false).
    bool predict = true;
};

// The analysis of one module. `solver` reads the module it was given, so the
// module must outlive the result and stay unchanged while it is read.
struct Analysis {
    std::unique_ptr<Solver> solver;
    std::unique_ptr<ShapeAnalysis> shapes;
    std::vector<SiteFact> sites;
};

Analysis analyze(const il::Module& mod, const AnalysisOptions& opts);

}  // namespace bronze::wpo
