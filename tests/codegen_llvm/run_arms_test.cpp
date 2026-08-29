// A RUN OF READS AS TWO ARMS (src/codegen-llvm/llvm_run_arms.h), read off
// hand-built IL.
//
// The plan is where every decision is taken — which runs become groups, what
// the join owes the block after it, whose root slot the fast arm may leave
// alone — and every one of those is exact on eight instructions and arithmetic
// on counts anywhere else. What the ARMS then emit is pinned against bytes in
// tests/oracle/cases/recv_proof_run_arms.js, which is where a wrong answer here
// shows up as a use-after-move under GC stress.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "codegen-llvm/llvm_live_roots.h"
#include "codegen-llvm/llvm_recv_proof.h"
#include "codegen-llvm/llvm_run_arms.h"
#include "il/il.h"

using namespace bronze;
using namespace bronze::codegen_llvm;

namespace {

constexpr uint32_t kNamedKey = 16;  // "foo", which is no index and so no member

il::Module makeModule() {
    il::Module module;
    module.name = "run_arms_test";
    for (uint32_t i = 0; i < 16; ++i) module.keyConstants.push_back(std::to_string(i));
    module.keyConstants.push_back("foo");
    return module;
}

struct FuncBuilder {
    il::Function func;
    il::ValueId next = 0;

    FuncBuilder() {
        func.name = "f";
        func.returnType = il::Type::Dynamic;
    }

    il::ValueId param(il::Type type = il::Type::Dynamic) {
        il::Param p;
        p.name = "p" + std::to_string(next);
        p.type = type;
        func.params.push_back(p);
        return next++;
    }

    size_t block() {
        il::Block b;
        b.id = static_cast<il::BlockId>(func.blocks.size());
        func.blocks.push_back(b);
        return func.blocks.size() - 1;
    }

    size_t push(size_t b, il::Instruction i) {
        func.blocks[b].instructions.push_back(std::move(i));
        return func.blocks[b].instructions.size() - 1;
    }

    // A constant-index read: the shape a run is made of.
    il::ValueId read(size_t b, il::ValueId recv, uint32_t index) {
        il::Instruction i;
        i.op = il::Op::PropGet;
        i.type = il::Type::Dynamic;
        i.result = next++;
        i.operands = {recv};
        i.keyIndex = index;
        push(b, std::move(i));
        return next - 1;
    }

    // A NAMED read, which is no run member and which collects.
    il::ValueId namedRead(size_t b, il::ValueId recv) {
        il::Instruction i;
        i.op = il::Op::PropGet;
        i.type = il::Type::Dynamic;
        i.result = next++;
        i.operands = {recv};
        i.keyIndex = kNamedKey;
        push(b, std::move(i));
        return next - 1;
    }

    il::ValueId alloc(size_t b) {
        il::Instruction i;
        i.op = il::Op::CreateObject;
        i.type = il::Type::Dynamic;
        i.result = next++;
        push(b, std::move(i));
        return next - 1;
    }

    il::ValueId isNumber(size_t b, il::ValueId of) {
        il::Instruction i;
        i.op = il::Op::IsNumber;
        i.type = il::Type::Bool;
        i.result = next++;
        i.operands = {of};
        push(b, std::move(i));
        return next - 1;
    }

    void jump(size_t b, size_t to) {
        il::Instruction i;
        i.op = il::Op::Jump;
        i.target.block = static_cast<il::BlockId>(to);
        push(b, std::move(i));
    }

    void branch(size_t b, il::ValueId cond, size_t thenBlock, size_t elseBlock) {
        il::Instruction i;
        i.op = il::Op::Branch;
        i.operands = {cond};
        i.target.block = static_cast<il::BlockId>(thenBlock);
        i.elseTarget.block = static_cast<il::BlockId>(elseBlock);
        push(b, std::move(i));
    }

    void ret(size_t b, il::ValueId v) {
        il::Instruction i;
        i.op = il::Op::Ret;
        i.operands = {v};
        push(b, std::move(i));
    }

    void finish() { func.valueCount = next; }
};

bool holds(const std::vector<il::ValueId>& list, il::ValueId v) {
    for (il::ValueId x : list) {
        if (x == v) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("a run whose members are consecutive becomes one group") {
    //   b0: %1 = create.object      the safepoint that gives %1 a slot
    //       %2 = prop.get %0, "0"   ] one run, three members, no gap
    //       %3 = prop.get %0, "1"   ]
    //       %4 = prop.get %0, "2"   ]
    //       %5 = is.number %2
    //       %6 = is.number %1
    //       ret %2
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();
    const size_t b0 = fb.block();
    const il::ValueId live = fb.alloc(b0);
    const il::ValueId e0 = fb.read(b0, recv, 0);
    const il::ValueId e1 = fb.read(b0, recv, 1);
    const il::ValueId e2 = fb.read(b0, recv, 2);
    const il::ValueId guard = fb.isNumber(b0, e0);
    fb.isNumber(b0, live);
    fb.ret(b0, e0);
    fb.finish();
    (void)guard;

    const RunArmPlan arms = planRunArms(module, fb.func);
    REQUIRE(arms.groups.size() == 1u);
    const RunArmGroup& group = arms.groups[0];
    CHECK(group.block == 0u);
    CHECK(group.first == 1u);
    CHECK(group.last == 3u);
    CHECK(group.receiver == recv);
    CHECK(group.maxIndex == 2u);
    CHECK(group.index == std::vector<uint32_t>{0, 1, 2});
    CHECK(group.result == std::vector<il::ValueId>{e0, e1, e2});
    CHECK(arms.startAt(0, 1) == 0u);
    CHECK(arms.startAt(0, 2) == RunArmPlan::kNoGroup);
    CHECK(arms.memberAt(0, 3) == 0u);
    CHECK(arms.memberAt(0, 4) == RunArmPlan::kNoGroup);
}

TEST_CASE("a value live across a group is restored at the join and read there") {
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();
    const size_t b0 = fb.block();
    const il::ValueId live = fb.alloc(b0);
    const il::ValueId e0 = fb.read(b0, recv, 0);
    fb.read(b0, recv, 1);
    fb.read(b0, recv, 2);
    fb.isNumber(b0, e0);
    fb.isNumber(b0, live);
    fb.ret(b0, e0);
    fb.finish();

    const LiveRootPlan plan = planLiveRoots(fb.func, planRunArms(module, fb.func));
    REQUIRE(plan.arms.groups.size() == 1u);
    // `live` is defined before the group, wanted after it, and the group's slow
    // arm can move it — so the join has to hand it forward and both arms have to
    // supply it.
    CHECK(plan.needsSlot[live] == 1);
    CHECK(holds(plan.arms.groups[0].restore, live));
    // And having handed it forward, the use after the group reads the register
    // the join phi'd rather than going back to the slot.
    CHECK(plan.anchor(0, 5, 0) == 0u);
    // The same for a result: the guard chain after the run reads registers.
    CHECK(plan.anchor(0, 4, 0) == 0u);
}

TEST_CASE("a result no reader can reach through its slot needs no store on the fast arm") {
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();
    const size_t b0 = fb.block();
    fb.alloc(b0);
    const il::ValueId e0 = fb.read(b0, recv, 0);
    const il::ValueId e1 = fb.read(b0, recv, 1);
    const il::ValueId e2 = fb.read(b0, recv, 2);
    fb.isNumber(b0, e0);
    fb.isNumber(b0, e1);
    // %e2 becomes an access RECEIVER, and llvm_ops_access.cpp hands a receiver's
    // slot ADDRESS to the static-slot publish — so this one slot has to be
    // current whatever the anchors say.
    const il::ValueId named = fb.namedRead(b0, e2);
    fb.ret(b0, named);
    fb.finish();

    const LiveRootPlan plan = planLiveRoots(fb.func, planRunArms(module, fb.func));
    REQUIRE(plan.arms.groups.size() == 1u);
    CHECK(plan.armLocal(e0));
    CHECK(plan.armLocal(e1));
    CHECK_FALSE(plan.armLocal(e2));
}

TEST_CASE("a run with anything between two of its members gets no group") {
    //   A named `prop.set` is TRANSPARENT to a run's proof (llvm_recv_proof.h),
    //   so the run still spans it — and its members are no longer consecutive,
    //   which is exactly the case the arms refuse.
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();
    const il::ValueId other = fb.param();
    const size_t b0 = fb.block();
    const il::ValueId e0 = fb.read(b0, recv, 0);
    {
        il::Instruction i;
        i.op = il::Op::PropSet;
        i.operands = {other, e0};
        i.keyIndex = kNamedKey;
        fb.push(b0, std::move(i));
    }
    const il::ValueId e1 = fb.read(b0, recv, 1);
    fb.ret(b0, e1);
    fb.finish();

    // The run is still there — this is about the ARMS refusing it, not about the
    // proof refusing it.
    const BlockRunPlan runs = planBlockRuns(module, fb.func, 0);
    CHECK(runs.reads.at(0).run == 0u);
    CHECK(runs.reads.at(2).run == 0u);
    CHECK(planRunArms(module, fb.func).groups.empty());
}

TEST_CASE("a run of one is no group") {
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();
    const size_t b0 = fb.block();
    const il::ValueId e0 = fb.read(b0, recv, 0);
    fb.ret(b0, e0);
    fb.finish();
    CHECK(planRunArms(module, fb.func).groups.empty());
}

TEST_CASE("a block emitted between the anchor and the group leaves no register to restore") {
    //   b0: jump b2
    //   b1: %2 = is.number %0        the SLOW COPY, reached only from b2's else
    //       ret %0
    //   b2: %3 = prop.get %1, "0"    ] the group
    //       %4 = prop.get %1, "1"    ]
    //       %5 = is.number %3
    //       br %5, b3, b1
    //   b3: %6 = is.number %0
    //       ret %0
    //
    // A guarded region's slow copy is a LOW-numbered block reachable only from
    // the bottom of the fast copy (src/lower/guard_region.h), and blocks are
    // emitted in index order — so b1's own reload of %0 runs between b0, which
    // the meet names as %0's anchor, and b2, which wants to hand %0's register
    // to its join. The register b1 left dominates neither arm, and a plan that
    // put %0 on the restore list would be asking the join to phi it.
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId out = fb.param();
    const il::ValueId src = fb.param();
    const size_t b0 = fb.block();
    const size_t b1 = fb.block();
    const size_t b2 = fb.block();
    const size_t b3 = fb.block();
    fb.jump(b0, b2);
    fb.isNumber(b1, out);
    fb.ret(b1, out);
    const il::ValueId e0 = fb.read(b2, src, 0);
    fb.read(b2, src, 1);
    const il::ValueId guard = fb.isNumber(b2, e0);
    fb.branch(b2, guard, b3, b1);
    fb.isNumber(b3, out);
    fb.ret(b3, out);
    fb.finish();

    const LiveRootPlan plan = planLiveRoots(fb.func, planRunArms(module, fb.func));
    REQUIRE(plan.arms.groups.size() == 1u);
    CHECK(plan.arms.groups[0].block == 2u);
    // %0 is live across the group and does have a slot — what it does not have
    // is a register the emitter still holds.
    CHECK(plan.needsSlot[out] == 1);
    CHECK(plan.arms.groups[0].restore.empty());
    CHECK(plan.arms.groups[0].restoreAnchor.size() == plan.arms.groups[0].restore.size());
    // And every use of it goes back to the slot, in the slow copy and after the
    // group alike.
    CHECK(plan.anchor(1, 0, 0) == LiveRootPlan::kReload);
    CHECK(plan.anchor(3, 0, 0) == LiveRootPlan::kReload);
}

TEST_CASE("a value the join phi'd is read from the phi and not from a slot nobody wrote") {
    //   b0: %2 = prop.get %0, "0"    ] group 0, whose results are arm-local
    //       %3 = prop.get %0, "1"    ]
    //       %4 = is.number %2
    //       jump b1
    //   b1: %5 = prop.get %1, "0"    ] group 1, which carries %2 across itself
    //       %6 = prop.get %1, "1"    ]
    //       %7 = is.number %2
    //       ret %2
    //
    // Group 0's fast arm writes %2's slot NOWHERE — no use reloads it, so
    // `armLocalSlot` lets the store go. Group 1's slow arm spills it on the way
    // in and its join phis it, which is the register the block after the join
    // holds. Naming group 0's block as the anchor there would send that use to
    // the slot instead, and the slot has never been written.
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recvA = fb.param();
    const il::ValueId recvB = fb.param();
    const size_t b0 = fb.block();
    const size_t b1 = fb.block();
    const il::ValueId e0 = fb.read(b0, recvA, 0);
    fb.read(b0, recvA, 1);
    fb.isNumber(b0, e0);
    fb.jump(b0, b1);
    fb.read(b1, recvB, 0);
    fb.read(b1, recvB, 1);
    fb.isNumber(b1, e0);
    fb.ret(b1, e0);
    fb.finish();

    const LiveRootPlan plan = planLiveRoots(fb.func, planRunArms(module, fb.func));
    REQUIRE(plan.arms.groups.size() == 2u);
    CHECK(holds(plan.arms.groups[1].restore, e0));
    CHECK(plan.armLocal(e0));
    // The join is in b1, so the uses after it read b1's phi.
    CHECK(plan.anchor(1, 2, 0) == 1u);
    CHECK(plan.anchor(1, 3, 0) == 1u);
}

TEST_CASE("a register survives into a block every predecessor carries it into") {
    //   b0: %0 = create.object
    //       %1 = create.object      the safepoint that gives %0 its slot
    //       %2 = is.number %0       reloads, so %0's register is current after it
    //       br %2, b1, b2
    //   b1: jump b3
    //   b2: jump b3
    //   b3: %3 = is.number %0       two predecessors, one anchor, no reload
    //       ret %0
    //
    // This is `multiplyMatrices`'s bail block in miniature: sixteen guard blocks
    // branch to it and the block that defined everything it collects dominates
    // all sixteen.
    FuncBuilder fb;
    const size_t b0 = fb.block();
    const size_t b1 = fb.block();
    const size_t b2 = fb.block();
    const size_t b3 = fb.block();
    const il::ValueId v = fb.alloc(b0);
    fb.alloc(b0);
    const il::ValueId cond = fb.isNumber(b0, v);
    fb.branch(b0, cond, b1, b2);
    fb.jump(b1, b3);
    fb.jump(b2, b3);
    fb.isNumber(b3, v);
    fb.ret(b3, v);
    fb.finish();

    const LiveRootPlan plan = planLiveRoots(fb.func);
    CHECK(plan.needsSlot[v] == 1);
    CHECK(plan.anchor(0, 2, 0) == LiveRootPlan::kReload);  // the reload that anchors it
    CHECK(plan.anchor(3, 0, 0) == 0u);                     // and the meet that carries it
}
