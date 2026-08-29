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
// to what it was. `BRONZE_GUARDED_REGION_STATS=1` prints one line to stderr,
// and `BRONZE_GUARDED_REGION_TRACE=1` one line per function that built or
// refused something — the counters say how often a reason fired, and only the
// trace says which functions, which is what decides whether a reason is worth a
// rule.
//
// ---------------------------------------------------------------------------
// THE ENTRY REGION
//
// A region does not have to be a loop. `h = b0` and `R = every block` is a
// region by the same definition — no edge from outside enters anywhere but the
// entry — and it is the one that covers a straight-line kernel like
// `Matrix4.multiplyMatrices`, which has no loop at all and is thirty-two
// property reads feeding a hundred and twenty-eight coercions.
//
// It is built by PREPENDING A PREHEADER and then running the loop machinery
// unchanged: a new empty block 0 whose only instruction is `jump b1`, with
// every old block shifted up by one. That makes `(b0_old, all old blocks)` a
// region with exactly one outside predecessor, which is the shape everything
// below already knows how to duplicate — one entry edge to redirect, one place
// to convert values on, one trampoline target when an entry guard fails. The
// preheader is a single unconditional jump and the backend folds it away.
//
// Selected only when the function has NO NATURAL LOOP AT ALL. Not "no loop
// region was built": a loop that was refused was refused for a reason — a
// handler in it, two entry edges — and a whole-function duplication would
// contain that same loop and duplicate the shape the refusal was about. A
// function with a loop is a loop function; this is for the ones without.
//
// Resume machines are refused by name (`il::Function::isResumeBody`), not by
// shape. A generator's body dispatches on a resume index at its entry, so
// every block is reachable from `b0` and the region definition is satisfied;
// what is wrong with duplicating it is that its live values cross suspensions
// in the FRAME rather than in SSA, so a promoted double cannot survive a yield
// and the copy is pure growth.
//
// One thing an entry region owes that a loop region does not: EVERY GUARD POINT
// MUST BE IN A BLOCK THAT DOMINATES WHAT IT REACHES. An entry region's slow copy
// has no entry of its own — the fast copy is the function's — so the only way
// into it is a trampoline, and the only block a trampoline lands in is the tail
// of a split. Everything the slow copy defines before a split is therefore
// orphaned, and the mechanism that re-supplies those values is the tail's
// parameters plus a rename in the blocks that read them.
//
// The parameters take care of themselves: a value defined before the split and
// read after it is live at the split, and the tail takes one for every value
// live there. So does a value defined in a block the prefix orphans whole — a
// block that cannot reach the split but whose definition is read past it
// dominates the split block, so that value is live at the split as well. What
// is a condition is the RENAME, which is found on a block's dominator chain:
// the split block has to dominate every region block reachable from it.
//
// The header satisfies that by the region's own definition, and used to be the
// whole rule. It is not the only block that does — the join after a DEFAULTED
// PARAMETER dominates everything under it too, which is what makes
// `Quaternion.setFromEuler` reachable at all: `(euler, update = true)` puts a
// branch in front of the block its six sines and cosines are computed in.
//
// The orphaned prefix is then pruned. Dominance is not defined over a block the
// entry cannot reach, so a rewrite that kept it could not be certified — and
// the validator now distinguishes "no definition at all" from "a function
// parameter", which is the shape a pruned block leaves behind.
//
// ---------------------------------------------------------------------------
// CHECKED UNBOXES ARE CANDIDATES TOO
//
// `*`, `-`, `/` and `%` over unproven operands take the numeric arm
// (`lower_expr_binary.cpp`): each operand gets a CHECKED `unbox.f64` — a tag
// test with a `ToNumber` call on the miss, `canThrow` and `canCollect` both
// true — and the result is an `f64`. So the arithmetic is already unboxed and
// the closure over `Add/Sub/Mul/Div/Mod/Pow/Neg` finds nothing: what is
// `Dynamic` is the OPERAND OF THE UNBOX.
//
// So a checked `unbox.f64 %v` makes `%v` a candidate. Behind the guard the
// unbox is not rewritten to a raw one — it is DELETED, and its result becomes
// the single raw unbox the guard emitted. Thirty-two guards then answer a
// hundred and twenty-eight coercions, each of which was an exception check and
// a safepoint, and the fast copy holds thirty-two bitcasts instead.
//
// ---------------------------------------------------------------------------
// GUARD COALESCING
//
// A guard's placement is the LATEST point that dominates its promoted uses.
// Inside the defining block that point is in front of the first use there; when
// every use is in a block BELOW — six sines and cosines computed in a header and
// consumed only in `switch` arms, which is `Quaternion.setFromEuler` — it is the
// end of the defining block, in front of the terminator. That still reaches
// them: the fast copy of one block is a chain of parts whose only exits are the
// guard branches, and a failed guard leaves for the slow copy and never comes
// back, so the last part of a block's chain dominates the fast copy of every
// block that block dominates. A use in a block the definition does NOT dominate
// has no site at all and refuses the region (`refusedPlacement`) — in practice
// that is an unreachable region block, since an entry region is every block of
// the function and dominance is not defined over one the entry cannot reach.
//
// Placing each candidate's guard at its own first use is correct and, on a
// straight-line kernel, quadratic: thirty-two guards interleaved with the
// arithmetic means thirty-two block splits, thirty-two trampolines, and each
// trampoline carrying every partial product live at that point.
//
// The rule instead is COALESCED, per block: the guard point for a set of
// candidates is the first promoted use of ANY of them, and every candidate
// whose definition precedes that point is guarded there — one chain of
// `is.number` + `br`, every failing edge landing on ONE trampoline. Candidates
// defined after that point take the next coalesced point, and so on.
//
// Guarding a value earlier than its own first use costs nothing: `is.number`
// reads bits, so moving it earlier moves no observable effect, and inside one
// block every use is unconditional anyway. What it buys on
// `multiplyMatrices` is the whole point of the pass there: the thirty-two
// reads stay in ONE block (the receiver proof, `llvm_recv_proof.h`, builds its
// runs from adjacent constant-index reads and stops at a block end), then one
// chain of thirty-two tests, then the entire arithmetic in one block, and one
// trampoline that carries no promoted value at all because every partial
// product is computed after it.
//
// An end-of-block point is just another point in this rule and gets no
// preference. A candidate whose uses are all below and whose definition
// PRECEDES the block's first promoted use joins that earlier point — the same
// "guarding earlier is free" that the paragraph above rests on. One whose
// definition comes after it cannot: a guard in front of its own definition has
// nothing to read, so the later point is forced rather than chosen.
// `setFromEuler` is the first shape: nine values, one chain, one trampoline.

// What the pass did, counted. Every refusal is counted by REASON, because a
// pass that proves nothing emits exactly the code it replaced and the whole
// suite still passes — the counters are what separate "the fast copy is
// correct" from "the fast copy is ever built".
struct GuardRegionStats {
    uint32_t functions = 0;   // functions examined
    // The two region KINDS, counted where the region is proposed rather than
    // where it is taken, so that `refused*` below adds up against them. What
    // was actually built is `duplicated`; these two minus it is the refusals.
    uint32_t regions = 0;       // innermost natural loops proposed
    uint32_t entryRegions = 0;  // whole-function regions proposed
    uint32_t duplicated = 0;  // regions a fast copy was actually built for
    uint32_t guards = 0;      // `is.number` instructions emitted
    uint32_t guardPoints = 0;  // places a block was split for a guard chain
    uint32_t elidedBox = 0;   // candidates whose def was `box.f64`: no guard
    uint32_t elidedPin = 0;   // candidates a `pin.guard number` already proved
    // Checked `unbox.f64`s the fast copy did not need: their operand was
    // guarded, so the coercion is the raw unbox the guard already emitted. The
    // ratio of this to `guards` is what an entry region is bought for.
    uint32_t unboxFolded = 0;
    uint32_t promoted = 0;    // values carried as f64 in a fast copy
    uint32_t blocksAdded = 0;
    uint32_t blocksPruned = 0;  // blocks the duplication left unreachable

    // Refusals, by the thing that refused. `handler` and `singleEntry` are
    // properties of the CFG, `nonNumeric` and `tooFew` of the candidate
    // closure, `growth` of the caps, and the last two of the SSA shape the
    // duplication would have had to repair.
    uint32_t refusedHandler = 0;
    uint32_t refusedSingleEntry = 0;
    uint32_t refusedNonNumeric = 0;
    uint32_t refusedTooFew = 0;
    uint32_t refusedGrowth = 0;
    // A candidate with a promoted use in a block its definition does not
    // dominate. There is no single point in the region that reaches every such
    // use and is reached by the definition, so there is no guard site at all —
    // as opposed to `refusedEntrySplit`, where a site exists and the entry
    // region cannot enter its own slow copy at it.
    uint32_t refusedPlacement = 0;
    uint32_t refusedEntrySplit = 0;  // an entry region wanting a split outside its header
    uint32_t refusedSsa = 0;         // the rewrite would not have been dominance-correct
    uint32_t refusedMachine = 0;    // a generator/async resume body, refused by name
    // The region's one outside predecessor is already a copy of an earlier
    // region in this same function, so the entry chain's re-passes of that
    // edge's arguments would cross a copy boundary (il.h `CopyClass`).
    uint32_t refusedCopyPred = 0;
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
    // UseGuard: the COALESCED guard point this candidate was assigned to, which
    // is at or before its own first promoted use and at or after its def.
    // PinElided: the first promoted use, where the bare raw unbox goes — the
    // pin has to stand immediately in front of THAT, not in front of a point
    // some other candidate chose.
    il::BlockId guardBlock = il::kNoBlock;
    uint32_t guardIndex = 0;
};

// One selected region, and everything the rewrite needs to know about it.
struct RegionPlan {
    // The whole-function region, whose `entryPred` is a synthesised preheader
    // rather than a block the function had. Only the counters and the caps read
    // it; the rewrite is the same rewrite.
    bool entryRegion = false;
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
    // COALESCED guard chain sits in front of. Only `UseGuard` sites split — a
    // `PinElided` one emits a bare `unbox.f64 raw` and needs no edge — and
    // several candidates share one entry here.
    std::vector<std::vector<uint32_t>> splitsOf;  // by block id

    // Everything a guard licenses: promotable arithmetic whose result is a
    // candidate, plus every checked `unbox.f64` of one. It is the numerator of
    // the profitability ratio, and counting the unboxes is what makes a
    // straight-line kernel — whose arithmetic is already `f64` behind them —
    // worth anything at all.
    uint32_t promotedUseCount = 0;
    uint32_t guardCount = 0;
};

// A CHECKED `unbox.f64 %v` with `%v` dynamic by `types`: ToNumber (7.1.4) over
// a value nothing proved, which can call a `valueOf`, can throw, and is
// followed by an exception check and a safepoint. `%v` is the candidate; the
// result is already an `f64`. Shared because SELECTION counts these as the work
// a guard licenses and the REWRITE deletes them.
bool isCheckedUnboxOf(const il::Instruction& inst, const std::vector<il::Type>& types);

// A copy of `fn` with an empty preheader in front of it: a new block 0 holding
// only `jump b1`, every other block shifted up by one. The whole-function
// region needs one outside predecessor of its header and a function's entry
// block has none.
il::Function withPreheader(const il::Function& fn);

// Selects the whole-function region of `prepped` — a function `withPreheader`
// has already been applied to. False when there is none to take, with the
// reason counted in `stats`.
bool selectEntryRegion(const il::Function& prepped, GuardRegionStats& stats, RegionPlan& plan);

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
