#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "il/il.h"

namespace bronze::lower {

// GUARDED NUMERIC REGIONS: an IL -> IL pass that duplicates a loop into a copy
// where the `dynamic` values feeding `+ - * / %` are tested ONCE and then
// carried as `f64`.
//
// What costs ten times is not the tag test. `add %a, %b` over two boxed
// operands already emits an inline number/number arm in the backend
// (llvm_arith.cpp, `branchIfBothNumbers`); what costs is that the RESULT is
// `il::Type::Dynamic`, so `planFrame` gives it a GC root slot and every use
// reloads out of it. This pass adds no test the backend was not already
// performing. It hoists the per-operation test into a per-region one and spends
// the answer on the REPRESENTATION: the loop-carried value becomes `f64`, which
// is a strictly stronger answer than anything `planRepr`'s lattice can produce,
// and which `planFrame` already rewards with no root at all.
//
// There is no deopt and no OSR here. The guard is a branch, and what its false
// edge leads to is the IL that would have existed anyway — the original region,
// split at the point the guard sits at, entered with the values the fast copy
// had computed so far, re-boxed. Control leaves the fast copy at most once per
// region entry and never comes back, so the fallback cannot thrash.
//
// It is an IL -> IL pass and not a second lowering because lowering hands out
// module-global identity as it goes — inline-cache indices, static-slot cells,
// tagged-template sites — and a second pass over the same source would hand out
// a second set of each. A COPIED instruction shares its `icIndex` (correct: same
// receiver, same shape, and the entry stays warm) and shares its template site
// (required: 13.2.8.4 gives one template object per source site).
//
// It reads only the IL, so it runs identically with and without inference. That
// is what lets the oracle suite compare the two modes byte for byte: the
// transformation is meaning-preserving, so both modes must still print the same
// bytes, and under `--no-infer` it fires on MORE values.
//
// Lives in `src/lower` on the precedent `bigint_reach` sets — not everything
// here is AST -> IL. Move the pair to `src/opt` when a second IL -> IL pass
// lands.
//
// THE SEAM is `BRONZE_NO_GUARDED_REGIONS=1`, read by the COMPILER once per
// process: with it set the pass selects nothing and the module is byte-identical
// to what it was. `BRONZE_GUARDED_REGION_STATS=1` prints one line to stderr.

// What the pass did, counted. Every refusal is counted by REASON, because a
// pass that proves nothing emits exactly the code it replaced and the whole
// suite still passes — the counters are what separate "the fast copy is
// correct" from "the fast copy is ever built".
struct GuardRegionStats {
    uint32_t functions = 0;   // functions examined
    uint32_t regions = 0;     // natural loops that survived selection
    uint32_t duplicated = 0;  // regions a fast copy was actually built for
    uint32_t guards = 0;      // `is.number` instructions emitted
    uint32_t elidedBox = 0;   // candidates whose def was `box.f64`: no guard
    uint32_t elidedPin = 0;   // candidates a `pin.guard number` already proved
    uint32_t promoted = 0;    // values carried as f64 in a fast copy
    uint32_t blocksAdded = 0;

    // Refusals, by the thing that refused. `handler` and `singleEntry` are
    // properties of the CFG, `nonNumeric` and `tooFew` of the candidate
    // closure, `growth` of the caps, and the last two of the SSA shape the
    // duplication would have had to repair.
    uint32_t refusedHandler = 0;
    uint32_t refusedSingleEntry = 0;
    uint32_t refusedNonNumeric = 0;
    uint32_t refusedTooFew = 0;
    uint32_t refusedGrowth = 0;
    uint32_t refusedPlacement = 0;  // a guard site with no promoted use in its own block
    uint32_t refusedSsa = 0;        // the rewrite would not have been dominance-correct
};

// Rewrites every function of `module` in place. Returns true when it changed
// anything. `stats` is optional.
//
// Meaning-preserving by construction, and checked: a function whose rewrite
// would leave a use its definition does not dominate is discarded whole and the
// original kept (`refusedSsa`), so a shape this chunk cannot repair is a missed
// optimisation rather than a miscompile.
bool applyGuardedRegions(il::Module& module, GuardRegionStats* stats = nullptr);

// THE SEAM, read once and cached. Exposed so that the caller can skip the pass
// entirely rather than only its effects.
bool guardedRegionsDisabled();

// Prints the counters to stderr under `BRONZE_GUARDED_REGION_STATS=1`, and does
// nothing otherwise.
void guardRegionStatsReport(const GuardRegionStats& stats);

// ---------------------------------------------------------------------------
// The two halves, split where the question changes: `guard_region_select.cpp`
// answers "which loop, which values, where does each guard go", reading the
// function and writing nothing; `guard_region_build.cpp` performs the rewrite.
// The plan below is the whole of what crosses between them.
// ---------------------------------------------------------------------------

// How a candidate's `f64` form is obtained in the fast copy.
enum class CandidateKind : uint8_t {
    // The def is `box.f64 %x`: the f64 form IS `%x`, and there is nothing to
    // test. This is what makes a loop's entry edge hand over a double instead
    // of a box, and what makes `sum = sum + x` need no guard on `sum`.
    BoxElide,
    // A `pin.guard %v, number` stands immediately in front of the first
    // promoted use. The claim is already checked, one instruction of reach,
    // exactly as `storeValueRepr` reads one (llvm_repr.cpp).
    PinElided,
    // Defined outside the region, so it dominates both copies: one guard on the
    // entry edge covers every iteration.
    EntryGuard,
    // Defined inside the region by something that is not promoted arithmetic —
    // a `prop.get`, a call, an env read. Guarded at its FIRST PROMOTED USE.
    UseGuard,
    // Defined by promoted arithmetic, or a block parameter of a region block:
    // it is already an f64 in the fast copy and no test can be owed on it.
    Promoted,
};

struct Candidate {
    il::ValueId value = il::kNoValue;
    CandidateKind kind = CandidateKind::Promoted;
    // BoxElide: the boxed operand, which is the f64 form.
    il::ValueId boxSource = il::kNoValue;
    // UseGuard / PinElided: where the guard (or the bare raw unbox) goes — the
    // index of the first promoted use inside the def's own block.
    il::BlockId guardBlock = il::kNoBlock;
    uint32_t guardIndex = 0;
};

// One selected region, and everything the rewrite needs to know about it.
struct RegionPlan {
    il::BlockId header = il::kNoBlock;
    // The region's blocks, ascending. `inRegion` is the same set by block id.
    std::vector<il::BlockId> blocks;
    std::vector<bool> inRegion;
    // The single block outside the region with an edge into the header. There
    // is exactly one; a header with two is refused rather than mis-duplicated,
    // because a preheader is a later chunk's job.
    il::BlockId entryPred = il::kNoBlock;

    // Candidates by value id, ascending, plus the same set as a lookup.
    std::vector<Candidate> candidates;
    std::vector<uint8_t> isCandidate;  // by ValueId; uint8_t so it is addressable

    // Split points: for each region block, the ascending instruction indices a
    // guard sits in front of. Only `UseGuard` sites split — a `PinElided` one
    // emits a bare `unbox.f64 raw` and needs no edge.
    std::vector<std::vector<uint32_t>> splitsOf;  // by block id

    uint32_t promotedArithCount = 0;
    uint32_t guardCount = 0;
};

// Selects the regions of one function, innermost-first, and returns them in
// header order. Empty when the function has none. Counts its own refusals.
//
// `alreadyBuilt` marks, by block id, the blocks a previous rewrite of this same
// function produced or consumed; a region touching one is skipped silently. It
// is what makes "one duplication per loop nest" true across a function with
// several nests: without it the second look at the function finds the natural
// loop the trampoline edge made out of the SLOW copy and duplicates that, which
// is the region this pass just built, a second time.
std::vector<RegionPlan> selectRegions(const il::Function& fn, GuardRegionStats& stats,
                                      const std::vector<uint8_t>& alreadyBuilt);

// Applies one plan to `fn`. Returns false when the rewrite would not have been
// dominance-correct, in which case `fn` is untouched. `alreadyBuilt` is
// rewritten to the new block numbering with everything this rewrite created or
// consumed marked.
bool buildGuardedRegion(il::Function& fn, const RegionPlan& plan, GuardRegionStats& stats,
                        std::vector<uint8_t>& alreadyBuilt);

}  // namespace bronze::lower
