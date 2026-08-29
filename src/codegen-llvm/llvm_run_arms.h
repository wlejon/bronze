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
//   a MEMBER of a covered run — a constant-index read (llvm_recv_proof.h) or a
//   constant-index Array element store (llvm_array_store_proof.h). Both proven
//   arms are unconditional given the run's own proof: a GEP and a load, a GEP
//   and a store. The two store runs that are NOT covered are named below.
//
//   a DUPLICABLE instruction — one that neither collects nor throws and whose
//   emission is a straight line: the machine constants, f64 and i32 arithmetic,
//   the number predicates, a non-string box, a raw or nullish-widened unbox.
//   The fast arm holds a COPY of it and the slow arm holds the original, so
//   every value the span defines is phi'd at the join like a member result.
//
// Anything else ends the span where it stands. A `prop.set` whose only
// safepoint-free arms are the three bare stores still has a miss arm that
// calls, and a typed-array store's proven arm tests the VALUE — a condition
// that is not known until the arm is already running — so neither can be an
// unconditional step of a fast arm, and neither is one.
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
// PROGRAM ORDER IS PRESERVED EXACTLY. The fast arm emits the span's
// instructions in the order they appear, so `m.copy(m)` — where the source
// array IS the destination — reads element i, writes element i, reads element
// i+1, in the same sequence the slow arm does. Nothing is hoisted, so nothing
// has to be proven not to alias.
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
// A member whose site carries a STATIC SLOT claim is refused too. That claim
// emits its own guard in front of the cache (llvm_static_slot.h) and so takes
// priority over the proven element access today; a group would silently reverse
// the two, and a reversal is not something to leave to an accident of order.
//
// THE SEAMS. `BRONZE_NO_RUN_ARMS=1` plans no groups at all AND puts the
// live-root anchor pass back on its sole-predecessor rule, because the two are
// one feature: the arms exist to make a run collect-free, and the anchor rule
// there is what lets the blocks after it read the registers that leaves.
// `BRONZE_NO_RUN_ARMS_INTERLEAVED=1` keeps the arms but refuses every span that
// is not one run's members and nothing else, which is the shape that stood
// before the span rule — so the two features A/B separately out of one binary.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "il/il.h"

namespace bronze::codegen_llvm {

// One run a group covers, and the ladder the group emits for it at its head.
struct RunArmProof {
    enum class Kind : uint8_t { Read, ArrayStore };

    Kind kind = Kind::Read;
    // The run this proof is, and the largest index its one length test clears —
    // both straight from the block's run plan, so the proof the group emits is
    // the proof the per-member shape would have emitted.
    uint32_t run = 0;
    uint32_t maxIndex = 0;
    il::ValueId receiver = il::kNoValue;
};

// One instruction of the span, in program order.
struct RunArmStep {
    static constexpr uint32_t kNoProof = UINT32_MAX;

    // Where the instruction sits in its block.
    uint32_t inst = 0;
    // The proof this step spends, or `kNoProof` for a duplicated non-member.
    uint32_t proof = kNoProof;
    // A member's element index; the value a store member writes.
    uint32_t index = 0;
    il::ValueId value = il::kNoValue;
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

// Whether the fast arm may hold a COPY of this instruction: it can neither
// collect nor throw, and its emission is a straight line with no edge out of
// the arm. Named here rather than at the emitter because the planner and the
// emitter must agree about it exactly — the planner decides the span and the
// emitter duplicates whatever the planner put in it.
bool runArmDuplicable(const il::Instruction& inst);

}  // namespace bronze::codegen_llvm
