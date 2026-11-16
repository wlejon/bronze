#pragma once

// THE DEOPT SITES of a module: every `resume.point` of a generic function G,
// numbered across the module. Each has an exit counter in the module's
// per-thread data (`__bronze_deopt_counts`, after one RETIRE word per G),
// which G's resume twin bumps at the entry the point gave it: every `leave`
// and `resume.call` that resumes there counts, and nothing on S's fast path
// does (il2mir build_resume_twins). The runtime prints the table at exit under
// BRONZE_DEOPT_STATS=1 (runtime/deopt_stats.cpp; docs/deopt-stats.md).

#include <cstdint>
#include <string>
#include <vector>

#include "il/il.h"

namespace bronze::codegen {

struct DeoptSite {
    uint32_t function = 0;  // G's index in Module::functions
    uint32_t resumeId = 0;  // the point's id within G
    uint32_t generic = 0;   // G's rank among the module's generic functions with points
};

struct DeoptSites {
    std::vector<DeoptSite> sites;  // by function index, then id: a site's rank is its slot
    // Per rank, G's index in Module::functions.
    std::vector<uint32_t> generics;
    // The speculated functions S that can retire (their G has points), by
    // index in Module::functions. Each has an ENTRY CELL in the image's
    // shared data (il2mir::entry_cell_name, emitDataSection): a word holding
    // S's address, which every direct call of S goes through (`call @S via
    // @cell`). G's twin stores S's retired entry there, a function that calls
    // G, once G's retire word reaches the limit (il2mir build_resume_twins),
    // so the calls are G's from then on and S's own code tests nothing.
    std::vector<uint32_t> retirable;
    // One line per site, in slot order, tab-separated: G's name, the resume
    // id, what the guards that resume there test and the instruction G
    // re-executes, and `file:line` of that instruction.
    std::string text;
};

// Everything above, from the module as the optimiser left it. `uniqueNames` is
// the backend's name per function (computeUniqueFunctionNames).
DeoptSites collectDeoptSites(const il::Module& module, const std::vector<std::string>& uniqueNames);

}  // namespace bronze::codegen
