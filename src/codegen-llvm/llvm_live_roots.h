#pragma once

// WHERE A VALUE HAS TO BE IN ITS ROOT SLOT, and where its register will do.
//
// The frame contract this refines is llvm_frame.h's: every `dynamic` value gets
// a slot, its def stores into it, and every use loads out of it again. The load
// is what makes a moved object safe to touch — but it is a load at EVERY use,
// including the ones no collection stands between. `Matrix4.multiplyMatrices`
// reads thirty-two elements, guards each for numberness, then unboxes each: the
// guard chain and the unbox chain cannot move the heap, so the thirty-two
// reloads the unbox chain performs re-read slots nothing has written since the
// guard chain read them.
//
// Two answers, and they are the same question asked at two scales:
//
//   needsSlot(v) — can a collection ever happen while v is still wanted? If no
//                  instruction that `il::canCollect` admits stands anywhere
//                  between v's def and its last use, nothing can move v, and v
//                  needs no slot at all: no root store at the def, no reload at
//                  any use, and one less slot in the frame. This is the same
//                  mechanism llvm_repr.h already uses for a value that can
//                  never be a pointer, reached from the other side — that plan
//                  asks what the BITS are, this one asks when they are LOOKED
//                  AT.
//
//   anchor(use)  — for a use of a value that does have a slot: has anything
//                  collected since the slot was last read into a register? If
//                  not, the use reads the register and emits no load. The
//                  answer is the IL block that register was written in, so the
//                  emitter can CHECK the plan rather than trust it.
//
// WHAT COUNTS AS A SAFEPOINT. `il::canCollect` (src/il/effects.cpp) is the
// oracle, and it is written the safe way round: the ops it excuses are the ones
// that provably allocate nothing and reach no user code, and everything else —
// including anything added tomorrow — collects. Two edges it deliberately does
// not describe are added here, because both reach an ALLOCATION the oracle is
// not asked about:
//
//   the exception edge — a `canThrow` instruction's unwind target. A throw
//                        mints its error object, so a value the handler reads
//                        has to be in its slot before the instruction that
//                        raises, not merely before the next call.
//   the pin barrier    — `Op::PinGuard`'s branching form answers no to both
//                        `canCollect` and `canThrow` on purpose (llvm_pin.h):
//                        its violating arm raises and LEAVES, so nothing on the
//                        kept path is stale after it. But that arm still mints
//                        a TypeError on the way to the handler, so the handler's
//                        live values must be rooted in front of the guard.
//
// WHY A USE MAY SKIP ITS RELOAD ONLY ALONG A SOLE-PREDECESSOR CHAIN. The
// register a skipped reload reads is an LLVM value, and a value must dominate
// its use. A block with one predecessor is dominated by it, so a chain of such
// blocks carries a register forward the way llvm_recv_proof.h carries a derived
// pointer forward across the same shape. A join is where two different
// registers could reach one use, and there the value comes out of its slot
// again. Exception predecessors END a chain outright: a handler is entered from
// an arbitrary point inside its block, so the register state at its entry is
// not the state at the end of any block.
//
// The plan is a PURE FUNCTION OF THE IL, like `planFrame` and `planRepr` beside
// it: no LLVM value enters, so the whole module's frames can be laid out before
// any body is emitted, and a test can assert the answer against hand-built IL.
//
// WHAT A RUN-ARM GROUP CHANGES (llvm_run_arms.h). A group is a SPAN of element
// accesses and the arithmetic between them, emitted as one proof branch over
// two straight-line arms, and its join hands every value live across it forward
// in a register — the fast arm's own, or the slow arm's reload. Two answers
// follow from that and neither is a weakening of the rule above:
//
//   needsSlot is UNCHANGED. The slow arm really does collect at every member,
//   so a value live across the group really does need a slot to be forwarded
//   in. The group costs no frame slot it did not already cost.
//
//   anchor treats the group as ONE instruction that does not collect. That is
//   the fast arm's truth, and the join makes it the slow arm's truth as well.
//   `armLocal` below then names the span's definitions whose slot NOTHING
//   outside the slow arm ever reads, and the fast arm stores nothing for those
//   at all. A use INSIDE the same span is not an outside reader: it reloads on
//   the slow arm, which wrote the slot itself one instruction earlier, and it
//   reads the fast arm's register on the arm that wrote no slot at all.
//
// WHY THE ANCHOR MEETS ITS PREDECESSORS. A register survives into a block whose
// every predecessor carries the same one — not merely into a block with a
// single predecessor. `multiplyMatrices` bails to its slow copy from sixteen
// different guard blocks, and the block that collects the thirty-three values
// for that bail has sixteen predecessors and is dominated by the block that
// defined all of them. The meet is exact intersection, so a value that reaches
// the meet with one anchor reached it through the block that anchor names, on
// every path — which is what domination means and what makes the register legal
// there. A predecessor the walk has not reached yet (a back edge) contributes
// nothing, and an exception predecessor ends the chain as it always did.
//
// AND WHY THE MEET IS NOT ENOUGH ON ITS OWN. Domination says the register would
// be legal; it does not say the emitter still HOLDS it. The emitter keeps one
// register per value and emits blocks in INDEX order, which the CFG does not
// follow — a guarded region lays its slow copy out as a low-numbered block that
// only the bottom of the fast copy branches to (src/lower/guard_region.h), so
// that copy's own reloads run between the block an anchor names and the block
// that wants to read it, and leave the emitter holding a register from neither.
// `reload` catches that per use and goes to the slot. A run-arm group's join has
// no per-use fallback in front of it, and its results' slots are elided on the
// strength of no use ever reloading, so the same question is asked here once:
// an anchor survives the meet only while nothing emitted since has overwritten
// the register it names.
//
// THE SEAM. `BRONZE_NO_LIVE_ROOTS=1` returns the plan that roots every
// `dynamic` value and reloads at every use — exactly the contract that stood
// before this file — so the A/B is one environment variable out of one binary.
// `BRONZE_NO_RUN_ARMS=1` keeps this stage but puts the anchor back on the
// sole-predecessor rule, because the meet above is what the arms are for.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "codegen-llvm/llvm_run_arms.h"
#include "il/il.h"

namespace bronze::codegen_llvm {

struct LiveRootPlan {
    // A use that has to read its value out of the slot.
    static constexpr uint32_t kReload = UINT32_MAX;

    // Per `il::ValueId`: whether a collection can happen while the value is
    // still wanted. Empty means "root everything", which is what a
    // default-constructed plan says and what the seam returns.
    std::vector<uint8_t> needsSlot;

    // Per USE SITE, in the order the emitter visits them: an instruction's
    // operands, then its `target` block arguments, then its `elseTarget` ones.
    // `kReload`, or the IL block whose emission last wrote the register.
    std::vector<uint32_t> useAnchor;
    // Where block `b`'s instruction `i`'s uses begin in `useAnchor`:
    // useBase[blockBase[b] + i]. `useBase` carries one extra entry so the count
    // of any instruction's uses is a subtraction.
    std::vector<uint32_t> useBase;
    std::vector<uint32_t> blockBase;

    // Per `il::ValueId`: the value is a run-arm group's result whose root slot
    // is read NOWHERE but inside that group's own slow arm — no use of it
    // reloads, no handler can read it, and no access site takes its slot's
    // address. The fast arm may then leave the slot alone entirely, which is
    // the thirty-two root stores `multiplyMatrices` exists to lose. Empty means
    // "no value qualifies", which is what the seams return.
    std::vector<uint8_t> armLocalSlot;

    // The groups this plan was computed against, with their restore lists
    // filled in (llvm_run_arms.h). The emitter reads both from here so that the
    // groups it emits and the groups the anchors assume are the same objects.
    RunArmPlan arms;

    // Set when the seam is down. The plan is then the pre-stage contract, and
    // nothing downstream has to know which of the two it is holding.
    bool seamOff = false;

    // Whether the value takes a GC root slot at all. An empty plan roots
    // everything, so a caller with no plan gets the old behaviour.
    bool rooted(il::ValueId id) const {
        return needsSlot.empty() || id >= needsSlot.size() || needsSlot[id] != 0;
    }

    // Whether the fast arm of the group that defines this value may skip the
    // root store at its def. False for everything this plan does not describe.
    bool armLocal(il::ValueId id) const {
        return id < armLocalSlot.size() && armLocalSlot[id] != 0;
    }

    // The anchor for one use. `kReload` for anything this plan does not
    // describe, so a caller that miscounts uses loses speed and not soundness.
    uint32_t anchor(size_t block, size_t inst, size_t use) const {
        if (block + 1 >= blockBase.size()) return kReload;
        const size_t gi = blockBase[block] + inst;
        if (gi + 1 >= useBase.size()) return kReload;
        const size_t at = useBase[gi] + use;
        if (at >= useBase[gi + 1]) return kReload;
        return useAnchor[at];
    }
};

// Computes one function's plan. Same IL in, same plan out.
//
// The first form plans no run-arm groups, which is the shape a test that is
// about rooting alone wants; the second takes the groups the emitter will emit
// and fills their restore lists on the way through.
LiveRootPlan planLiveRoots(const il::Function& func);
LiveRootPlan planLiveRoots(const il::Function& func, RunArmPlan arms);

// Every function's plan, indexed like `il::Module::functions`. The whole module
// at once because the frame layouts are, and for the same reason: a caller has
// to size its frame for a callee it has not emitted yet.
std::vector<LiveRootPlan> planLiveRoots(const il::Module& module);

// Whether the seam is down for this invocation (`BRONZE_NO_LIVE_ROOTS=1`).
// Read once and cached.
bool liveRootsDisabled();

}  // namespace bronze::codegen_llvm
