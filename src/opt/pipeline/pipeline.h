#pragma once

// THE OPTIMISER PIPELINE: what every compile whose output is kept runs between
// lowering and the backend (`bronze build`, `bronze il`, eval::buildModule).
// One function, one order:
//
//   1. verify the module as lowered (OptimizeOptions::verify);
//   2. the speculation step (opt/speculate), only with a profile: functions
//      with decisive evidence get a speculated copy S, their generic G gets
//      resume points;
//   3. the redundancy pass over the whole module (opt/redundancy), with
//      guards hoisted out of S's loops to the loop-entry resume points step 2
//      placed (speculate::loopEntryResume answers its PreheaderResumeFn), and
//      the resume points of G no `leave` names removed;
//   4. representation selection (opt/repr) over S, which ends with the
//      redundancy pass again under the same options; skipped when nothing
//      was speculated, since it changes only S;
//   5. verify again.
//
// IDENTITY. Lowering stamps the module's harvest identity before this runs
// (lower_final_passes.cpp): il::numberSlowSites, then Module::icLayoutDigest.
// Nothing here renumbers a slow site or an inline-cache index or recomputes
// the digest: S's generic leftovers keep G's indices, and the only new IC
// sites (speculate's prototype-method reads, Context::newIcSite) are appended
// at and above the lowered `icSiteCount`. So the digest a module carries is
// the one of the module as lowered, which is what a build without a profile
// (no S, the build that is harvested) and a build reading its harvest both
// compute: speculate's Evidence compares the harvest against
// icLayoutDigest(module) and icSiteCount on the untouched module, before it
// changes anything. A module holding a speculated function is never
// harvested itself (brass_backend_sections.cpp spentHarvest).
//
// Nothing is printed here; formatOptimizeStats renders the counts.

#include <cstdint>
#include <string>

#include "il/il.h"
#include "opt/inline/inline.h"
#include "opt/redundancy/redundancy.h"
#include "opt/repr/repr.h"
#include "opt/spec/profile.h"
#include "opt/speculate/speculate.h"
#include "support/diagnostics.h"

namespace bronze::opt {

struct OptimizeOptions {
    // Run il::verify before and after. On by default in a build with
    // assertions; a failure stops the pipeline (OptimizeResult::ok false).
    bool verify = true;
    // Where a verification failure is reported. Null: only `ok` says so.
    DiagnosticSink* diags = nullptr;
    speculate::SpeculateOptions speculate;
    inliner::InlineOptions inlining;
    // The redundancy pass's own knobs (RedundancyOptions).
    uint32_t maxRounds = 3;
    bool hoistLoops = true;
    // Threads the per-function steps may use (0: the machine's); the
    // module they leave does not depend on it.
    size_t threads = 0;
};

struct OptimizeResult {
    // False when a verification failed; the module is then as far as the
    // pipeline got and must not be compiled.
    bool ok = true;
    // A profile was given (step 2 ran).
    bool profileRead = false;
    speculate::SpeculateStats speculate;
    // Step 3.
    inliner::InlineStats inlined;
    RedundancyStats redundancy;
    // Step 4, its closing redundancy run in `repr.redundancy`.
    ReprStats repr;

    // The profile read is this module's: its harvest was taken from a build
    // that numbered its sites as this one does, or it speculated a function.
    // What eval::ModuleBuildResult::feedbackApplied reports. A harvest of a
    // run in which nothing ran often enough to speculate still fits: the
    // module is the one that harvest gives, and a host that took "nothing
    // speculated" for "another build's file" would drop it and harvest again
    // at every launch.
    bool profileFits() const {
        return profileRead && (speculate.harvestMatched != 0 || speculate.functionsSpeculated != 0);
    }
};

OptimizeResult optimizeModule(il::Module& module, const spec::TypeProfile* profile,
                              const OptimizeOptions& options = {});

// A compact summary of `result`, a few lines (`bronze build --opt-stats`).
std::string formatOptimizeStats(const OptimizeResult& result);

}  // namespace bronze::opt
