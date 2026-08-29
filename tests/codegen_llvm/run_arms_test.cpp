// A SPAN OF ELEMENT ACCESSES AS TWO ARMS (src/codegen-llvm/llvm_run_arms.h),
// read off hand-built IL.
//
// The plan is where every decision is taken — which spans become groups, which
// runs one test covers, what the join owes the block after it, whose root slot
// the fast arm may leave alone — and every one of those is exact on eight
// instructions and arithmetic on counts anywhere else. What the ARMS then emit
// is pinned against bytes in tests/oracle/cases/recv_proof_run_arms.js and
// recv_proof_run_arms_interleaved.js, which is where a wrong answer here shows
// up as a use-after-move under GC stress.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "codegen-llvm/llvm_live_roots.h"
#include "codegen-llvm/llvm_recv_proof.h"
#include "codegen-llvm/llvm_run_arms.h"
#include "codegen-llvm/llvm_store_proof.h"
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

    // A constant-index read: the shape a read run is made of.
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

    // A constant-index write: the shape an Array store run is made of.
    void store(size_t b, il::ValueId recv, uint32_t index, il::ValueId value) {
        il::Instruction i;
        i.op = il::Op::PropSet;
        i.operands = {recv, value};
        i.keyIndex = index;
        push(b, std::move(i));
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

    il::ValueId constF64(size_t b, double v) {
        il::Instruction i;
        i.op = il::Op::ConstF64;
        i.type = il::Type::F64;
        i.result = next++;
        i.immF64 = v;
        push(b, std::move(i));
        return next - 1;
    }

    il::ValueId addF64(size_t b, il::ValueId x, il::ValueId y) {
        il::Instruction i;
        i.op = il::Op::Add;
        i.type = il::Type::F64;
        i.result = next++;
        i.operands = {x, y};
        push(b, std::move(i));
        return next - 1;
    }

    // `recv[base + k] = value`: the shape a typed-array store run is made of,
    // spelled the way the guarded-region pass spells it — the index boxed off
    // an add, or the base boxed bare for k == 0.
    void typedStore(size_t b, il::ValueId recv, il::ValueId base, uint32_t k,
                    il::ValueId value) {
        const il::ValueId index =
            k == 0 ? boxF64(b, base) : boxF64(b, addF64(b, base, constF64(b, k)));
        il::Instruction i;
        i.op = il::Op::ElemSet;
        i.type = il::Type::Void;
        i.operands = {recv, index, value};
        push(b, std::move(i));
    }

    // A machine-number multiply: duplicable, so a span spans it.
    il::ValueId mulF64(size_t b, il::ValueId x, il::ValueId y) {
        il::Instruction i;
        i.op = il::Op::Mul;
        i.type = il::Type::F64;
        i.result = next++;
        i.operands = {x, y};
        push(b, std::move(i));
        return next - 1;
    }

    il::ValueId boxF64(size_t b, il::ValueId of) {
        il::Instruction i;
        i.op = il::Op::Box;
        i.type = il::Type::Dynamic;
        i.boxType = il::Type::F64;
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

std::vector<uint32_t> indices(const RunArmGroup& group) {
    std::vector<uint32_t> out;
    for (const RunArmStep& s : group.steps) {
        if (s.proof != RunArmStep::kNoProof) out.push_back(s.index);
    }
    return out;
}

// Where in its block each step the GATE holds sits: the closure of what the
// span's typed-array stores write.
std::vector<uint32_t> gated(const RunArmGroup& group) {
    std::vector<uint32_t> out;
    for (const RunArmStep& s : group.steps) {
        if (s.gate) out.push_back(s.inst);
    }
    return out;
}

std::vector<il::ValueId> storedValues(const RunArmGroup& group, RunArmProof::Kind kind) {
    std::vector<il::ValueId> out;
    for (const RunArmStep& s : group.steps) {
        if (s.proof != RunArmStep::kNoProof && group.proofs[s.proof].kind == kind) {
            out.push_back(s.value);
        }
    }
    return out;
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
    REQUIRE(group.proofs.size() == 1u);
    CHECK(group.proofs[0].kind == RunArmProof::Kind::Read);
    CHECK(group.proofs[0].receiver == recv);
    CHECK(group.proofs[0].maxIndex == 2u);
    CHECK(indices(group) == std::vector<uint32_t>{0, 1, 2});
    CHECK(group.result == std::vector<il::ValueId>{e0, e1, e2});
    CHECK(arms.startAt(0, 1) == 0u);
    CHECK(arms.startAt(0, 2) == RunArmPlan::kNoGroup);
    CHECK(arms.memberAt(0, 3) == 0u);
    CHECK(arms.memberAt(0, 4) == RunArmPlan::kNoGroup);
}

TEST_CASE("a read run interleaved with an Array store run is ONE group under TWO proofs") {
    //   `Matrix4.copy` in miniature: te[i] = me[i], twice. Neither run's members
    //   are consecutive; together they are the whole span.
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId src = fb.param();
    const il::ValueId dst = fb.param();
    const size_t b0 = fb.block();
    const il::ValueId e0 = fb.read(b0, src, 0);
    fb.store(b0, dst, 0, e0);
    const il::ValueId e1 = fb.read(b0, src, 1);
    fb.store(b0, dst, 1, e1);
    fb.ret(b0, dst);
    fb.finish();

    const RunArmPlan arms = planRunArms(module, fb.func);
    REQUIRE(arms.groups.size() == 1u);
    const RunArmGroup& group = arms.groups[0];
    CHECK(group.first == 0u);
    CHECK(group.last == 3u);
    CHECK(group.memberCount() == 4u);
    REQUIRE(group.proofs.size() == 2u);
    CHECK(group.proofs[0].kind == RunArmProof::Kind::Read);
    CHECK(group.proofs[0].receiver == src);
    CHECK(group.proofs[1].kind == RunArmProof::Kind::ArrayStore);
    CHECK(group.proofs[1].receiver == dst);
    // Only the reads define anything, and the steps keep program order — read 0,
    // write 0, read 1, write 1 — which is what makes `m.copy(m)` come out the
    // same on both arms.
    CHECK(group.result == std::vector<il::ValueId>{e0, e1});
    CHECK(group.steps[0].proof == 0u);
    CHECK(group.steps[1].proof == 1u);
    CHECK(group.steps[1].value == e0);
    CHECK(group.steps[3].value == e1);
}

TEST_CASE("one receiver read and written is still one group") {
    //   `m.copy(m)`: the source IS the destination. Two proofs about one object,
    //   and the span holds them in program order, so element i is read before it
    //   is written and after the element before it was.
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId self = fb.param();
    const size_t b0 = fb.block();
    const il::ValueId e0 = fb.read(b0, self, 0);
    fb.store(b0, self, 0, e0);
    const il::ValueId e1 = fb.read(b0, self, 1);
    fb.store(b0, self, 1, e1);
    fb.ret(b0, self);
    fb.finish();

    const RunArmPlan arms = planRunArms(module, fb.func);
    REQUIRE(arms.groups.size() == 1u);
    REQUIRE(arms.groups[0].proofs.size() == 2u);
    CHECK(arms.groups[0].proofs[0].receiver == self);
    CHECK(arms.groups[0].proofs[1].receiver == self);
    CHECK(arms.groups[0].last == 3u);
}

TEST_CASE("arithmetic between two members is duplicated into the span") {
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();
    const il::ValueId x = fb.param(il::Type::F64);
    const size_t b0 = fb.block();
    const il::ValueId e0 = fb.read(b0, recv, 0);
    const il::ValueId scaled = fb.mulF64(b0, x, x);
    const il::ValueId e1 = fb.read(b0, recv, 1);
    fb.ret(b0, e1);
    fb.finish();

    const RunArmPlan arms = planRunArms(module, fb.func);
    REQUIRE(arms.groups.size() == 1u);
    const RunArmGroup& group = arms.groups[0];
    CHECK(group.first == 0u);
    CHECK(group.last == 2u);
    CHECK(group.steps.size() == 3u);
    CHECK(group.steps[1].proof == RunArmStep::kNoProof);
    // The multiply defines a value like a member does, so the join phis it and
    // the block after the group reads what the two arms agreed on.
    CHECK(group.result == std::vector<il::ValueId>{e0, scaled, e1});
}

TEST_CASE("a collecting non-member ends the span in front of it") {
    //   The run itself ends at the named read (it collects), so what stands in
    //   front is a whole run and becomes a group; the read after it opens a run
    //   of one, which is no run at all.
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();
    const il::ValueId other = fb.param();
    const size_t b0 = fb.block();
    fb.read(b0, recv, 0);
    fb.read(b0, recv, 1);
    fb.namedRead(b0, other);
    const il::ValueId last = fb.read(b0, recv, 2);
    fb.ret(b0, last);
    fb.finish();

    const RunArmPlan arms = planRunArms(module, fb.func);
    REQUIRE(arms.groups.size() == 1u);
    CHECK(arms.groups[0].first == 0u);
    CHECK(arms.groups[0].last == 1u);
    CHECK(arms.memberAt(0, 2) == RunArmPlan::kNoGroup);
    CHECK(arms.memberAt(0, 3) == RunArmPlan::kNoGroup);
}

TEST_CASE("a run cut by something the fast arm cannot hold gets no group") {
    //   A named `prop.set` is TRANSPARENT to a run's proof (llvm_recv_proof.h),
    //   so the run still spans it — but its three bare-store arms are only three
    //   of its arms, and the miss arm calls. The fast arm cannot hold it, the
    //   span stops in front of it, and a member of the same run stands after the
    //   span: the group is refused rather than cut.
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

TEST_CASE("a proof whose receiver the span itself defines is refused") {
    //   The ladders stand at the group's HEAD, so a receiver the span defines is
    //   one the ladder would read before it exists.
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();
    const il::ValueId x = fb.param(il::Type::F64);
    const size_t b0 = fb.block();
    fb.read(b0, recv, 0);
    fb.read(b0, recv, 1);
    const il::ValueId boxed = fb.boxF64(b0, x);
    fb.store(b0, boxed, 0, x);
    fb.store(b0, boxed, 1, x);
    fb.ret(b0, boxed);
    fb.finish();

    CHECK(planRunArms(module, fb.func).groups.empty());
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

TEST_CASE("a read consumed INSIDE its own span still needs no store on the fast arm") {
    //   `copy`'s reads go straight into `copy`'s stores. That use reloads on the
    //   slow arm — which wrote the slot one instruction earlier — and reads the
    //   fast arm's register on the arm that wrote no slot at all, so it is not a
    //   reader the fast arm owes anything to.
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId src = fb.param();
    const il::ValueId dst = fb.param();
    const size_t b0 = fb.block();
    const il::ValueId e0 = fb.read(b0, src, 0);
    fb.store(b0, dst, 0, e0);
    const il::ValueId e1 = fb.read(b0, src, 1);
    fb.store(b0, dst, 1, e1);
    fb.ret(b0, dst);
    fb.finish();

    const LiveRootPlan plan = planLiveRoots(fb.func, planRunArms(module, fb.func));
    REQUIRE(plan.arms.groups.size() == 1u);
    CHECK(plan.armLocal(e0));
    CHECK(plan.armLocal(e1));
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

TEST_CASE("a read run threaded through a typed-array store run is ONE gated group") {
    //   `Matrix4.toArray` in miniature: three constant-index reads off an Array,
    //   three affine-index writes into a view, and the index arithmetic between
    //   them. One group, two proofs, and a GATE holding the loads whose results
    //   the stores write — and nothing else, because a store defines nothing for
    //   the closure to pull in.
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId src = fb.param();
    const il::ValueId dst = fb.param();
    const il::ValueId base = fb.param(il::Type::F64);
    const size_t b0 = fb.block();
    const il::ValueId e0 = fb.read(b0, src, 0);
    fb.typedStore(b0, dst, base, 0, e0);
    const il::ValueId e1 = fb.read(b0, src, 1);
    fb.typedStore(b0, dst, base, 1, e1);
    const il::ValueId e2 = fb.read(b0, src, 2);
    fb.typedStore(b0, dst, base, 2, e2);
    fb.ret(b0, dst);
    fb.finish();

    const RunArmPlan arms = planRunArms(module, fb.func);
    REQUIRE(arms.groups.size() == 1u);
    const RunArmGroup& group = arms.groups[0];
    CHECK(group.first == 0u);
    CHECK(group.last == 12u);
    CHECK(group.gated);
    REQUIRE(group.proofs.size() == 2u);
    CHECK(group.proofs[0].kind == RunArmProof::Kind::Read);
    CHECK(group.proofs[0].receiver == src);
    CHECK(group.proofs[0].maxIndex == 2u);
    CHECK(group.proofs[1].kind == RunArmProof::Kind::TypedStore);
    CHECK(group.proofs[1].receiver == dst);
    // The one length test is taken against this base and has to clear
    // `base + 2`, which is the largest offset any member writes.
    CHECK(group.proofs[1].base == base);
    CHECK(group.proofs[1].maxIndex == 2u);
    CHECK(indices(group) == std::vector<uint32_t>{0, 0, 1, 1, 2, 2});
    CHECK(storedValues(group, RunArmProof::Kind::TypedStore) ==
          std::vector<il::ValueId>{e0, e1, e2});
    // The reads, and only the reads: the box/add/const that make each index are
    // in the span but not in the gate, because no store writes one of them.
    CHECK(gated(group) == std::vector<uint32_t>{0, 3, 8});
}

TEST_CASE("a typed-array store of a value from outside the span leaves the gate empty") {
    //   Nothing inside the span produces what these stores write, so the gate
    //   holds no step at all — it is the value TESTS and the branch, and the
    //   value comes out of the slot the way any operand does.
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId dst = fb.param();
    const il::ValueId base = fb.param(il::Type::F64);
    const il::ValueId v = fb.param();
    const size_t b0 = fb.block();
    fb.typedStore(b0, dst, base, 0, v);
    fb.typedStore(b0, dst, base, 1, v);
    fb.ret(b0, dst);
    fb.finish();

    const RunArmPlan arms = planRunArms(module, fb.func);
    REQUIRE(arms.groups.size() == 1u);
    const RunArmGroup& group = arms.groups[0];
    // The leading `box.f64` of the base is a duplicable, and a duplicable does
    // not open a span — the first store does.
    CHECK(group.first == 1u);
    CHECK(group.last == 5u);
    CHECK(group.gated);
    CHECK(group.memberCount() == 2u);
    CHECK(gated(group).empty());
}

TEST_CASE("an offset the one length test cannot name ends the span in front of it") {
    //   `kMaxStoreOffset` is what makes the bound arithmetic obviously free of
    //   overflow, so a store past it is in no run — and a step that is neither a
    //   member nor duplicable stops the span. What the group covers is what its
    //   one test covers, which is offset 1 and not 65536.
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId src = fb.param();
    const il::ValueId dst = fb.param();
    const il::ValueId base = fb.param(il::Type::F64);
    const size_t b0 = fb.block();
    const il::ValueId e0 = fb.read(b0, src, 0);
    const il::ValueId e1 = fb.read(b0, src, 1);
    const il::ValueId e2 = fb.read(b0, src, 2);
    fb.typedStore(b0, dst, base, 0, e0);
    fb.typedStore(b0, dst, base, 1, e1);
    fb.typedStore(b0, dst, base, kMaxStoreOffset + 1, e2);
    fb.ret(b0, dst);
    fb.finish();

    const RunArmPlan arms = planRunArms(module, fb.func);
    REQUIRE(arms.groups.size() == 1u);
    const RunArmGroup& group = arms.groups[0];
    CHECK(group.first == 0u);
    CHECK(group.last == 8u);
    REQUIRE(group.proofs.size() == 2u);
    CHECK(group.proofs[1].kind == RunArmProof::Kind::TypedStore);
    CHECK(group.proofs[1].maxIndex == 1u);
    CHECK(arms.memberAt(0, 12) == RunArmPlan::kNoGroup);
    // Only the two reads the two proven stores write are gated; the third read
    // is inside the span but feeds a store the span does not hold.
    CHECK(gated(group) == std::vector<uint32_t>{0, 1});
}

TEST_CASE("an Array store and a typed-array store never share a span") {
    //   The gate HOISTS its loads above the stores between them, and only the
    //   typed-array store carries a proof that says it cannot be the Array those
    //   loads came off — `m.copy(m)` is the case that says an Array store can.
    //   So the second kind ends the span rather than joining it.
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId src = fb.param();
    const il::ValueId arr = fb.param();
    const il::ValueId dst = fb.param();
    const il::ValueId base = fb.param(il::Type::F64);
    const size_t b0 = fb.block();
    const il::ValueId e0 = fb.read(b0, src, 0);
    fb.store(b0, arr, 0, e0);
    const il::ValueId e1 = fb.read(b0, src, 1);
    fb.store(b0, arr, 1, e1);
    fb.typedStore(b0, dst, base, 0, e0);
    fb.typedStore(b0, dst, base, 1, e1);
    fb.ret(b0, arr);
    fb.finish();

    const RunArmPlan arms = planRunArms(module, fb.func);
    REQUIRE(arms.groups.size() == 2u);
    const RunArmGroup& first = arms.groups[0];
    CHECK(first.first == 0u);
    CHECK(first.last == 3u);
    CHECK_FALSE(first.gated);
    REQUIRE(first.proofs.size() == 2u);
    CHECK(first.proofs[0].kind == RunArmProof::Kind::Read);
    CHECK(first.proofs[1].kind == RunArmProof::Kind::ArrayStore);
    // The typed stores open a span of their own behind it, gated and holding
    // nothing else — so neither kind is lost, and no gate hoists a load over an
    // Array store.
    const RunArmGroup& second = arms.groups[1];
    CHECK(second.first == 5u);
    CHECK(second.last == 9u);
    CHECK(second.gated);
    REQUIRE(second.proofs.size() == 1u);
    CHECK(second.proofs[0].kind == RunArmProof::Kind::TypedStore);
    CHECK(gated(second).empty());
}

TEST_CASE("a store run whose base the span itself defines is refused") {
    //   The store ladder stands at the group's HEAD and takes its one length
    //   test against the base, so a base the span defines is one the ladder
    //   would read before it exists — the same rule the receivers are held to.
    il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId src = fb.param();
    const il::ValueId dst = fb.param();
    const il::ValueId x = fb.param(il::Type::F64);
    const size_t b0 = fb.block();
    const il::ValueId e0 = fb.read(b0, src, 0);
    const il::ValueId e1 = fb.read(b0, src, 1);
    const il::ValueId base = fb.mulF64(b0, x, x);
    fb.typedStore(b0, dst, base, 0, e0);
    fb.typedStore(b0, dst, base, 1, e1);
    fb.ret(b0, dst);
    fb.finish();

    CHECK(planRunArms(module, fb.func).groups.empty());
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
