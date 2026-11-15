#pragma once

// The jobs of the redundancy pass, as the driver (driver.cpp) calls them.
// Each takes the function as it stands, builds the analyses it needs, edits
// the function and answers whether it changed anything. None relies on an
// analysis another job built: every snapshot is rebuilt where it is used.

#include "il/il.h"
#include "opt/redundancy/redundancy.h"

namespace bronze::opt::redundancy {

struct JobContext {
    il::Module& module;
    il::Function& fn;
    const RedundancyOptions& options;
    RedundancyStats& stats;
};

// Jobs 1-3 in one dominator walk (value_walk.cpp): value numbering, constant
// folding and identities, test and branch folding, load elimination and
// store-to-load forwarding; then unreachable blocks go.
bool runValueWalk(JobContext& ctx);

// Job 4 (licm.cpp): loop-invariant code motion, guards included.
bool runLoopHoisting(JobContext& ctx);

// Job 5 (dead_stores.cpp): stores overwritten before anything could read them.
bool runDeadStores(JobContext& ctx);

// Job 6 (dead_code.cpp): unused instructions and block parameters, trivial
// block parameters, empty forwarding blocks, unreachable blocks.
bool runDeadCode(JobContext& ctx);

// Job 6's resume-point half (dead_code.cpp): on a generic function, with
// `removeUnnamedResumePoints`, every `resume.point` no `leave` names.
bool removeUnnamedResumePoints(JobContext& ctx);

}  // namespace bronze::opt::redundancy
