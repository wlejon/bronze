#pragma once

// ONE PROOF BRANCH, TWO STRAIGHT-LINE ARMS.
//
// llvm_recv_proof.h establishes a receiver proof once in front of a run and
// then spends it per member: each read is `ok ? load : ladder`, joined at its
// own block, with `ok` and `base` re-phi'd there for the member after it. That
// shape is correct and it is why the ladder is paid once — but it leaves every
// member a SAFEPOINT, because the member's own ladder arm reaches
// bronze_prop_get, which can run a getter and collect. The live-root plan
// (llvm_live_roots.h) must therefore assume a collection between any two reads
// of the run: every result takes a root slot, every def stores into it, and
// every use in the guard chain that follows loads out of it again. On
// `Matrix4.multiplyMatrices` that is thirty-two stores and sixty-five loads
// around thirty-two loads of actual data.
//
// A GROUP is a SPAN of consecutive instructions of one block emitted under a
// single test:
//
//     ok ? { load, store, mul, add, ... }   <- nothing that collects or throws
//        : { ladder, ladder, ... }          <- exactly today's per-member cache
//
// joined at one block with one phi per value the span defines. The fast arm is
// then a straight line that `il::canCollect` would call empty, and the whole
// group is one non-collecting unit as far as anything after it is concerned.
//
// WHAT MAY BE IN A SPAN. Two things, and nothing else:
//
//   a MEMBER of a covered run — a constant-index read (llvm_recv_proof.h), a
//   constant-index Array element store (llvm_array_store_proof.h), or an
//   affine-index typed-array store (llvm_store_proof.h). The first two proven
//   arms are unconditional given the run's own proof: a GEP and a load, a GEP
//   and a store. The third owes a test the GATE below pays for.
//
//   an OWN-SLOT READ — `position.x`, whose class layout proved a constant
//   instance slot (il.h, `staticSlot`). Its proven arm is a GEP at a
//   compile-time offset and a load, under a shape question the group asks once
//   at its head; llvm_static_slot.h states what the published cell and the
//   family stamp make that load stand for. This kind is here for the same
//   reason the interleaved span is: `Matrix4.compose` writes twelve constant
//   indices of `this.elements`, then `te[12] = position.x` three times, and a
//   named read that ENDS the span leaves those last four stores paying a ladder
//   each. Its receiver is proven PLAIN, and every other kind's is proven ARRAY
//   or TYPED ARRAY, so an own-slot read and any element access in the same span
//   are about different objects by construction.
//
//   a DUPLICABLE instruction — one that neither collects nor throws and whose
//   emission is a straight line: the machine constants, f64 and i32 arithmetic,
//   the number predicates, a non-string box, a raw or nullish-widened unbox.
//   The fast arm holds a COPY of it and the slow arm holds the original, so
//   every value the span defines is phi'd at the join like a member result.
//
// Anything else ends the span where it stands. A `prop.set` whose only
// safepoint-free arms are the three bare stores still has a miss arm that
// calls, so it is not an unconditional step of a fast arm and is not one.
//
// THE GATE, which is what lets a typed-array store be a member at all. That
// store's proven arm tests the VALUE for numberness (llvm_store_proof.h: a
// value that needs converting owes ToNumber, which runs user code), and the
// answer is not known at the group's head, so the test cannot simply join the
// AND of the proofs there. But in `Matrix4.toArray` — the shape this exists
// for — every stored value is a READ MEMBER of the same span, and a proven read
// is a GEP and a load with no safepoint in it. So the fast path is split in two:
//
//     ok ? { loads; is-number of each stored value }  -> gate
//        : ladders                                    -> slow
//     all-numeric ? { the span, stores included }     -> fast
//                 : ladders                           -> slow
//
// and the fast arm is unconditional again. NOTHING HAS BEEN STORED when the
// gate refuses, so the slow arm is entered from the head and from the gate
// alike and performs the whole span from the top — no resumable mid-span entry,
// and no store performed twice. The gate's loads are the only work redone, and
// a proven element load is pure: the proof says the receiver is a dense Array,
// which is what makes the load the observable answer in the first place.
//
// WHAT THE GATE HOLDS is the closure, inside the span, of what each stored
// value is made of: the read members and duplicable instructions that produce
// it, and nothing else. Those steps are HOISTED above the typed-array stores
// between them, which is the one reordering this file performs, and it is legal
// for a reason the receiver proofs supply: a read member's receiver was proven
// to be an Array and a store member's to be a TYPED ARRAY, so the two are
// different objects and the load cannot see the store. An ARRAY store carries
// no such proof about the read run's receiver — `m.copy(m)` is the counterexample
// — so a span never holds an Array store and a typed-array store at once, and
// the mix ends the span instead.
//
// WHY ONE TEST FOR SEVERAL RUNS. `Matrix4.copy` reads sixteen elements off one
// Array and writes them into another, interleaved; `multiplyMatrices` computes
// into sixteen constant-index stores off arithmetic. So a span may carry more
// than one run, and the group's test is the AND of every run's `ok`. Each
// ladder is a chain of loads and compares that allocates nothing, so all of
// them may stand at the group's head — and once they do, nothing inside the
// span can invalidate any of them: a duplicable instruction touches no object,
// a proven read touches none, and a proven Array store writes an element slot
// the run's own length and capacity guards already placed inside the block.
//
// PROGRAM ORDER IS PRESERVED EXACTLY, apart from the gate's hoist above. The
// fast arm emits the span's instructions in the order they appear, so
// `m.copy(m)` — where the source array IS the destination — reads element i,
// writes element i, reads element i+1, in the same sequence the slow arm does.
// The two stores keep that order between themselves whatever the gate did with
// the loads, which is why the hoist needs only the load-against-store argument
// and not a store-against-store one.
//
// WHAT THE ARMS OWE THE JOIN. A value defined before the group and live after
// it is in a register on the fast path and may have MOVED on the slow path. So
// the slow arm reloads every such value out of its slot before it branches, the
// fast arm hands over the register it already had, and the join phis the two —
// `restore` below is that list, and the live-root plan fills it, because "live
// after the group with a current register" is exactly the question that plan
// already answers. The values the span DEFINES are the same story with the fast
// arm's own registers on one side.
//
// A run whose first member in the span does not ESTABLISH the proof is a run
// chained in from an earlier block (llvm_recv_proof.h, `BlockRunPlan::continues`),
// and the span stops in front of it: the branch would have to test a proof that
// entered the block through a phi, which is exactly the per-member shape again.
//
// An ELEMENT member whose site carries a STATIC SLOT claim is refused. That
// claim emits its own guard in front of the cache (llvm_static_slot.h) and so
// takes priority over the proven element access today; a group would silently
// reverse the two, and a reversal is not something to leave to an accident of
// order. The own-slot kind above is the same rule read the other way round: a
// site with a claim and an INDEX key stays an element access under its own
// guard, and only a NAMED key becomes an own-slot step.
//
// THE SEAMS. `BRONZE_NO_RUN_ARMS=1` plans no groups at all AND puts the
// live-root anchor pass back on its sole-predecessor rule, because the two are
// one feature: the arms exist to make a run collect-free, and the anchor rule
// there is what lets the blocks after it read the registers that leaves.
// `BRONZE_NO_RUN_ARMS_INTERLEAVED=1` keeps the arms but refuses every span that
// is not one run's members and nothing else, which is the shape that stood
// before the span rule — so the two features A/B separately out of one binary.
// `BRONZE_NO_RUN_ARMS_TYPED_STORES=1` keeps the spans but refuses every
// typed-array store member, which puts the gate and everything downstream of it
// back to the shape that stood before it: `Matrix4.toArray` cut at its first
// store and planned no group at all. `BRONZE_NO_OWN_SLOT_STEP=1` refuses every
// own-slot step AND the run planner's carry across such a read — one seam for
// both halves, because a group cannot span both sides of a read the planner
// still treats as the end of a run, and a carry with no group to spend it on is
// register pressure paid for nothing.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "il/il.h"

namespace bronze::codegen_llvm {

// One run a group covers, and the ladder the group emits for it at its head.
struct RunArmProof {
    enum class Kind : uint8_t { Read, ArrayStore, TypedStore, OwnSlot };

    Kind kind = Kind::Read;
    // The run this proof is, and the largest index its one length test clears —
    // both straight from the block's run plan, so the proof the group emits is
    // the proof the per-member shape would have emitted. For a TypedStore the
    // index is an OFFSET off `base`, and the one length test clears
    // `base + maxIndex`. Both `0` for an OwnSlot, which is a claim about one
    // receiver's shape and belongs to no run.
    uint32_t run = 0;
    uint32_t maxIndex = 0;
    il::ValueId receiver = il::kNoValue;
    // The f64 SSA value a TypedStore run's indices are affine over, which its
    // ladder needs in a register at the group's head. `kNoValue` for the other
    // kinds, whose indices are constants in the plan.
    il::ValueId base = il::kNoValue;
    // An OwnSlot's guard, straight off the site (il.h): the module cell the
    // IDENTITY form compares the shape against, or the class subtree the FAMILY
    // form asks the shape's stamp about. Which of the two is `familyLo`, exactly
    // as it is at the site (llvm_static_slot.h, `StaticSite::family`).
    //
    // These are the proof's IDENTITY as well as its parameters. Two fields of
    // one object under one family stamp share a guard, because the stamp stands
    // for that class's whole declared field list at once; two identity sites
    // hold two cells, and each cell has to be compared for itself.
    uint32_t cellIndex = 0;
    uint32_t familyLo = il::Instruction::kNoFamily;
    uint32_t familySpan = 0;
};

// One instruction of the span, in program order.
struct RunArmStep {
    static constexpr uint32_t kNoProof = UINT32_MAX;

    // Where the instruction sits in its block.
    uint32_t inst = 0;
    // The proof this step spends, or `kNoProof` for a duplicated non-member.
    uint32_t proof = kNoProof;
    // Where on the receiver this step reaches: an element index for a Read or an
    // ArrayStore, an offset off `base` for a TypedStore, and the INSTANCE SLOT
    // for an OwnSlot. One field because it is one question — which position of
    // the proven receiver — and the proof beside it says in whose numbering.
    uint32_t index = 0;
    // The value a store member writes.
    il::ValueId value = il::kNoValue;
    // Whether the GATE holds this step: it produces, directly or through other
    // gate steps, a value some typed-array store member of this span writes.
    // Such a step is emitted once, ahead of the value tests, and the fast arm
    // reads the register it left rather than emitting it again.
    bool gate = false;
};

// One span emitted as two arms.
struct RunArmGroup {
    uint32_t block = 0;
    // The span, at consecutive indices `first..last`. Both ends are members:
    // a duplicable instruction outside the outermost members buys nothing.
    uint32_t first = 0;
    uint32_t last = 0;

    std::vector<RunArmProof> proofs;
    std::vector<RunArmStep> steps;
    // Every value the span DEFINES, in program order — member results and the
    // results of the duplicated instructions alike. The join phis all of them.
    std::vector<il::ValueId> result;
    // Values defined BEFORE the group, live after it, and carried in a register
    // the plan trusts — the fast arm hands its register to the join, the slow
    // arm reloads out of the slot. Filled by planLiveRoots.
    std::vector<il::ValueId> restore;
    // The block whose emission the plan believes wrote each restored value's
    // register, parallel to `restore`. The arms hand that register to a phi and
    // to a spill with no per-use fallback in front of either, so the emitter
    // checks the belief against its own `regBlock_` the way `reload` does —
    // a register the emitter no longer holds is a value that must come out of
    // its slot before the proof branches.
    std::vector<uint32_t> restoreAnchor;
    // Whether the span holds a typed-array store, and so whether the fast path
    // is split by a gate. Derived from `proofs` at planning time and kept here
    // because the emitter asks it once per group rather than once per step.
    bool gated = false;

    // How many steps spend a proof: what the group has to pay for.
    size_t memberCount() const {
        size_t n = 0;
        for (const RunArmStep& s : steps) {
            if (s.proof != RunArmStep::kNoProof) ++n;
        }
        return n;
    }
};

// One function's groups, addressed the way the live-root plan addresses uses:
// by block and instruction index.
struct RunArmPlan {
    static constexpr uint32_t kNoGroup = UINT32_MAX;

    std::vector<RunArmGroup> groups;
    // Where block `b`'s instructions begin in the two maps below.
    std::vector<uint32_t> blockBase;
    // The group that STARTS at this instruction, and the group whose SPAN this
    // instruction is inside. Both `kNoGroup` for everything else.
    std::vector<uint32_t> startOf;
    std::vector<uint32_t> memberOf;

    bool empty() const { return groups.empty(); }

    uint32_t startAt(size_t block, size_t inst) const { return lookup(startOf, block, inst); }
    uint32_t memberAt(size_t block, size_t inst) const { return lookup(memberOf, block, inst); }

private:
    uint32_t lookup(const std::vector<uint32_t>& map, size_t block, size_t inst) const {
        if (block + 1 >= blockBase.size()) return kNoGroup;
        const size_t at = blockBase[block] + inst;
        if (at >= blockBase[block + 1] || at >= map.size()) return kNoGroup;
        return map[at];
    }
};

// Plans one function's groups. Pure: the run plan it reads is itself a pure
// function of the module and the block, so the same IL gives the same groups
// and a test can assert them against hand-built IL.
RunArmPlan planRunArms(const il::Module& module, const il::Function& func);

// Whether the seam is down for this invocation (`BRONZE_NO_RUN_ARMS=1`). Read
// once and cached.
bool runArmsDisabled();

// Whether a span may hold anything but one run's own members
// (`BRONZE_NO_RUN_ARMS_INTERLEAVED=1` turns that off). Read once and cached.
bool interleavedRunArmsEnabled();

// Whether a span may hold a typed-array store member, and so a gate
// (`BRONZE_NO_RUN_ARMS_TYPED_STORES=1` turns that off). Read once and cached.
bool typedStoreRunArmsEnabled();

// Whether a span may hold an own-slot read, and so whether the run planner
// carries a proof across one (`BRONZE_NO_OWN_SLOT_STEP=1` turns both off). Read
// once and cached.
bool ownSlotStepEnabled();

// Whether this instruction is the NAMED OWN-SLOT READ the step above is about.
// Asked in one place because three callers have to agree exactly: the planner
// that admits the step, the run planner that spans the instruction instead of
// ending its runs, and llvm_prop_get.cpp, which hands the site's static hit
// block back as the join's proof-preserving edge for the sites where no group
// formed.
bool ownSlotRead(const il::Module& module, const il::Instruction& inst);

// Whether the fast arm may hold a COPY of this instruction: it can neither
// collect nor throw, and its emission is a straight line with no edge out of
// the arm. Named here rather than at the emitter because the planner and the
// emitter must agree about it exactly — the planner decides the span and the
// emitter duplicates whatever the planner put in it.
bool runArmDuplicable(const il::Instruction& inst);

// What the planner admitted, module-wide. A span rule is easy to believe in and
// hard to check — a rule that admits nothing emits exactly the code it replaced
// and every test still passes — so these count PLANNED GROUPS, which is the only
// thing that tells "the step is correct" from "the step is ever taken".
//
// Process-global for the reason llvm_repr.h's counters are: one `bronze build`
// is one module, and threading a counter through the planner to report a number
// nothing else reads would cost more in signatures than the number is worth.
struct RunArmStats {
    uint32_t groups = 0;
    uint32_t members = 0;
    // Groups holding at least one own-slot step, and the steps themselves.
    uint32_t ownSlotGroups = 0;
    uint32_t ownSlotSteps = 0;
};

RunArmStats& runArmStats();

// Prints the counters to stderr when `BRONZE_RUN_ARM_STATS=1`, and does nothing
// otherwise. Called once, after the module is emitted.
void runArmStatsReport();

// Whether every group holding an own-slot step names itself as it is planned
// (`BRONZE_RUN_ARM_STATS=2`). Read once and cached.
bool runArmStatsVerbose();

}  // namespace bronze::codegen_llvm
