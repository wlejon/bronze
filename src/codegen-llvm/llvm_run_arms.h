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
// A GROUP is a run whose members are CONSECUTIVE instructions of one block. For
// one of those the proof can be branched on ONCE:
//
//     ok ? { load, load, ... load }        <- no call, no allocation, no throw
//        : { ladder, ladder, ... ladder }  <- exactly today's per-member cache
//
// joined at one block with one phi per member result. The fast arm is then a
// straight line that `il::canCollect` would call empty, and the whole group is
// one non-collecting unit as far as anything after it is concerned.
//
// WHAT THE ARMS OWE THE JOIN. A value defined before the group and live after
// it is in a register on the fast path and may have MOVED on the slow path. So
// the slow arm reloads every such value out of its slot before it branches, the
// fast arm hands over the register it already had, and the join phis the two —
// `restore` below is that list, and the live-root plan fills it, because "live
// after the group with a current register" is exactly the question that plan
// already answers. The member results are the same story with the fast arm's
// loads on one side.
//
// WHY THE GROUP IS THE WHOLE RUN OR NOTHING. A run's SPAN may hold instructions
// that are not members — `Matrix4.copy` reads one array and writes another, so
// its reads and its stores interleave — and duplicating those into both arms
// would mean two copies of every store and a phi for every value they define.
// The rule here is the one that needs no such thing: a group is a run all of
// whose members sit at consecutive indices, and a run with anything between two
// of its members gets today's per-member emission unchanged. `multiplyMatrices`
// is thirty-two consecutive reads in two runs of sixteen and is the shape this
// exists for; `Matrix4.copy` keeps what it had.
//
// A run whose first member does not ESTABLISH the proof is a run chained in
// from an earlier block (llvm_recv_proof.h, `BlockRunPlan::continues`), and it
// gets no group: the branch would have to test a proof that entered the block
// through a phi, which is exactly the per-member shape again. The head of such
// a chain still gets its own group, and the tail spends the rejoined proof.
//
// A member whose site carries a STATIC SLOT claim is refused too. That claim
// emits its own guard in front of the cache (llvm_static_slot.h) and so takes
// priority over the proven element load today; a group would silently reverse
// the two, and a reversal is not something to leave to an accident of order.
//
// THE SEAM. `BRONZE_NO_RUN_ARMS=1` plans no groups at all AND puts the
// live-root anchor pass back on its sole-predecessor rule, because the two are
// one feature: the arms exist to make a run collect-free, and the anchor rule
// below is what lets the blocks after it read the registers that leaves.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "il/il.h"

namespace bronze::codegen_llvm {

// One run emitted as two arms.
struct RunArmGroup {
    uint32_t block = 0;
    // The member instructions, at consecutive indices `first..last`.
    uint32_t first = 0;
    uint32_t last = 0;
    // The run this group is, and the largest index its one length test clears —
    // both straight from the receiver-run plan, so the proof the group emits is
    // the proof the per-member shape would have emitted.
    uint32_t run = 0;
    uint32_t maxIndex = 0;
    il::ValueId receiver = il::kNoValue;
    // Per member, in order: the element index it reads and the value it defines.
    std::vector<uint32_t> index;
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

    size_t size() const { return index.size(); }
};

// One function's groups, addressed the way the live-root plan addresses uses:
// by block and instruction index.
struct RunArmPlan {
    static constexpr uint32_t kNoGroup = UINT32_MAX;

    std::vector<RunArmGroup> groups;
    // Where block `b`'s instructions begin in the two maps below.
    std::vector<uint32_t> blockBase;
    // The group that STARTS at this instruction, and the group this instruction
    // is a member of. Both `kNoGroup` for everything else.
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

// Plans one function's groups. Pure: the receiver-run plan it reads is itself a
// pure function of the module and the block, so the same IL gives the same
// groups and a test can assert them against hand-built IL.
RunArmPlan planRunArms(const il::Module& module, const il::Function& func);

// Whether the seam is down for this invocation (`BRONZE_NO_RUN_ARMS=1`). Read
// once and cached.
bool runArmsDisabled();

}  // namespace bronze::codegen_llvm
