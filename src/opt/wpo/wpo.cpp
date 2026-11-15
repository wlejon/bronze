#include "opt/wpo/wpo.h"

namespace bronze::wpo {

namespace {

// A prediction keeps the escaped objects a proof folds into the unknown, so
// its sets are as large as the program's whole escaped heap where a proof's
// are one unknown. A set past this many objects predicts no slot a guard could
// use anyway, and widening it keeps the solve as fast as the proof's.
constexpr size_t kPredictSetCap = 128;

}  // namespace

Analysis analyze(const il::Module& mod, const AnalysisOptions& opts) {
    SolverOptions o;
    o.hostGlobals = opts.hostGlobals;
    o.predict = opts.predict;
    if (opts.predict) o.setCap = kPredictSetCap;
    Analysis out;
    out.solver = std::make_unique<Solver>(mod, std::move(o));
    out.solver->run();
    out.shapes = std::make_unique<ShapeAnalysis>(*out.solver);
    out.shapes->run();
    out.sites = classifySites(*out.solver, *out.shapes);
    return out;
}

}  // namespace bronze::wpo
