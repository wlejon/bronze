// The pass's driver: the rule order within a round, the rounds to a fixed
// point, and the module walk.
//
// Within a round: rule 1 first (it answers uses of the boxes speculation
// placed and of those the previous round put at block tops), dead
// instructions next (a folded unbox must not count as a use of a box), then
// rule 2, whose new parameter boxes the next round's rule 1 folds. Rule 3
// runs once, last: it only moves boxes the other rules are done with.

#include "opt/core/cfg_edit.h"
#include "opt/repr/phases.h"
#include "opt/repr/repr.h"
#include "opt/repr/rewrite.h"

namespace bronze::opt {

void ReprStats::add(const ReprStats& o) {
    rounds += o.rounds;
    testsFolded += o.testsFolded;
    unboxesFolded += o.unboxesFolded;
    conversionsFolded += o.conversionsFolded;
    comparesTyped += o.comparesTyped;
    paramsUnboxed += o.paramsUnboxed;
    paramsWidened += o.paramsWidened;
    edgeConversions += o.edgeConversions;
    boxesSunkToExits += o.boxesSunkToExits;
    exitBoxClones += o.exitBoxClones;
    boxesSunk += o.boxesSunk;
    deadRemoved += o.deadRemoved;
    redundancy.add(o.redundancy);
}

namespace {

// Rounds of rules 1 and 2. A loop nest retypes its parameters in one solve;
// a further round is needed only where a fold exposes a new box to an edge
// (a `to.numeric` of a parameter's box, say), which nests shallowly.
constexpr uint32_t kMaxRounds = 4;

bool removeDead(repr::PhaseContext& ctx) {
    const size_t n = repr::removeDeadInstructions(ctx.fn);
    ctx.stats.deadRemoved += n;
    return n != 0;
}

}  // namespace

bool selectRepresentations(il::Module& module, il::Function& fn, ReprStats* stats) {
    if (!fn.isSpeculated() || fn.isExternal || fn.blocks.empty()) return false;
    ReprStats local;
    repr::PhaseContext ctx{module, fn, stats != nullptr ? *stats : local};

    bool any = removeUnreachableBlocks(fn) != 0;
    bool lastRoundChanged = false;
    for (uint32_t round = 0; round < kMaxRounds; ++round) {
        ++ctx.stats.rounds;
        bool changed = repr::foldBoxedUses(ctx);
        changed |= removeDead(ctx);
        changed |= repr::unboxBlockParams(ctx);
        lastRoundChanged = changed;
        any |= changed;
        if (!changed) break;
    }
    // The bound cut the rounds short: the boxes the last retyping placed
    // still get their uses folded.
    if (lastRoundChanged) {
        any |= repr::foldBoxedUses(ctx);
        any |= removeDead(ctx);
    }
    any |= repr::sinkBoxes(ctx);
    any |= removeDead(ctx);
    return any;
}

ReprStats selectRepresentationsModule(il::Module& module, const RedundancyOptions& cleanup) {
    ReprStats stats;
    // Indices, not iterators: nothing here adds functions, but nothing below
    // should have to know that.
    for (size_t i = 0; i < module.functions.size(); ++i) {
        il::Function& fn = module.functions[i];
        if (fn.isSpeculated()) selectRepresentations(module, fn, &stats);
    }
    stats.redundancy.add(runRedundancyModule(module, cleanup));
    return stats;
}

}  // namespace bronze::opt
