#include "opt/wpo/wpo.h"

#include <chrono>

#include "opt/wpo/census.h"
#include "opt/wpo/shape.h"
#include "opt/wpo/solver.h"

namespace bronze::wpo {

std::string statsReport(const il::Module& mod, const StatsOptions& opts) {
    SolverOptions solverOpts;
    solverOpts.hostGlobals = opts.hostGlobals;
    Solver solver(mod, solverOpts);
    solver.run();
    const auto start = std::chrono::steady_clock::now();
    ShapeAnalysis shapes(solver);
    shapes.run();
    const double shapeMillis =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const auto facts = classifySites(solver, shapes);
    CensusOptions censusOpts;
    censusOpts.perSite = opts.perSite;
    censusOpts.filter = opts.filter;
    return formatCensus(solver, shapes, facts, censusOpts, shapeMillis);
}

}  // namespace bronze::wpo
