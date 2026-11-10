#pragma once

// FUNCTION PLACEMENT: which functions of a module the backend lays out first
// in its text, in what order, and which last (il.h `hotFunctions` /
// `coldFunctions`; brass object_writer.hpp FunctionPlacement).
//
// An ahead-of-time module's speed depends on where its hot functions sit:
// laid out in module order they are scattered over megabytes of text, and
// which of them share L1 instruction-cache sets and op-cache lines is a draw
// that every edit anywhere in the module redraws (a few percent of cycles at
// equal instructions on the realworld scenes). Placement makes it a decision.
//
// With a profile (a harvest's samples, runtime/harvest_sampler.h, or a
// recorded profile's entry counts; opt/spec/feedback_plan.h):
//   - HOT: the most sampled functions, to 99% of the samples. What a
//     harvest's caches say ran is most of the program (a scene's setup runs
//     megabytes of code once); the samples say where the run spent its time.
//   - ordered by call chains (C3: each function, hottest first, joins the
//     chain of the caller with the heaviest edges into it, up to a size
//     bound; the edges are the callees the run's method sites named and the
//     IL's direct calls, a call in a loop weighing more), then the chains by
//     samples per byte;
//   - then, in module order, the functions a hot one calls that ran (the
//     samples saw them, or the harvest's caches filled in them) but that the
//     samples did not make hot: short callees seen only when a sample falls
//     on their entry. They follow the chains rather than join them, so the
//     sampled code stays dense;
//   - COLD: the module's top-level code, and the functions whose sites could
//     have filled and none did.
// Without one (none, a stale harvest, or one too short to hold a sample):
// nothing; the text stays in module order. Nothing here changes an
// instruction.

#include <string>

#include "il/il.h"

namespace bronze::spec {
struct FeedbackPlan;
}

namespace bronze::lower {

struct PlacementStats {
    bool profiled = false;
    uint32_t hot = 0;            // functions placed hot
    uint32_t cold = 0;
    uint32_t chains = 0;         // C3 clusters among the hot functions
    uint64_t hotInstructions = 0;
    uint64_t instructions = 0;
};

// Sets `mod.hotFunctions` / `mod.coldFunctions` for the module as the final
// passes left it. `plan` is the profile read against the probe, or null; a
// plan that matched nothing (a stale harvest) counts as none.
PlacementStats planFunctionPlacement(il::Module& mod, const spec::FeedbackPlan* plan);

std::string placementReport(const PlacementStats& stats);

}  // namespace bronze::lower
