// What the receiver-run planner will and will not span when a run's
// instructions are spread over several BLOCKS
// (src/codegen-llvm/llvm_recv_proof.h).
//
// The shape this is about is the guarded numeric region's: it splits a block
// after every value it guards, so three.js's `Vector3.applyMatrix4` — sixteen
// reads off one `m.elements` with `this.x = …` between them — arrives at the
// planner as seventeen blocks of one read each, and a per-block planner refused
// every one of them. A straight line of blocks in which each one has the
// previous as its ONLY predecessor and takes no parameters is dominated by its
// head, so a proof made in the head is a value every member may use with no phi
// at all, and the run spans them.
//
// Pinned on the PLAN, for the reason array_store_proof_test.cpp gives: the plan
// is where every decision here is taken. What the ladder does with a plan is
// pinned against bytes in tests/oracle/cases/recv_proof_chain.js.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "codegen-llvm/llvm_recv_proof.h"
#include "il/il.h"

using namespace bronze;
using namespace bronze::codegen_llvm;

namespace {

constexpr uint32_t kNamedKey = 16;  // "foo"

il::Module makeModule() {
    il::Module module;
    module.name = "run_chain_test";
    for (uint32_t i = 0; i < 16; ++i) module.keyConstants.push_back(std::to_string(i));
    module.keyConstants.push_back("foo");
    return module;
}

// A function under construction, block by block. Values are numbered across the
// whole function, as the IL's SSA numbers them.
struct FuncBuilder {
    il::Function func;
    il::ValueId next = 0;

    FuncBuilder() {
        func.name = "f";
        func.returnType = il::Type::Dynamic;
    }

    il::ValueId param() { return next++; }

    // Opens a block and answers its index. `fast` puts it in the fast copy of
    // region 0, which is what a guarded region's blocks are; a `Shared` block
    // is reached from both copies and never chains with either.
    size_t block(bool fast = true) {
        il::Block b;
        b.id = static_cast<il::BlockId>(func.blocks.size());
        b.copyClass = fast ? il::CopyClass::Fast : il::CopyClass::Shared;
        b.copyRegion = fast ? 0u : il::kNoCopyRegion;
        func.blocks.push_back(b);
        return func.blocks.size() - 1;
    }

    void addParam(size_t b) {
        il::BlockParam p;
        p.id = next++;
        p.type = il::Type::F64;
        func.blocks[b].params.push_back(p);
    }

    size_t read(size_t b, il::ValueId recv, uint32_t keyIndex) {
        il::Instruction i;
        i.op = il::Op::PropGet;
        i.type = il::Type::Dynamic;
        i.result = next++;
        i.operands = {recv};
        i.keyIndex = keyIndex;
        func.blocks[b].instructions.push_back(i);
        return func.blocks[b].instructions.size() - 1;
    }

    size_t store(size_t b, il::ValueId recv, uint32_t keyIndex, il::ValueId value) {
        il::Instruction i;
        i.op = il::Op::PropSet;
        i.type = il::Type::Void;
        i.operands = {recv, value};
        i.keyIndex = keyIndex;
        func.blocks[b].instructions.push_back(i);
        return func.blocks[b].instructions.size() - 1;
    }

    // The archetypal `il::canCollect`: it reaches user code, and every proof
    // standing over it dies.
    size_t call(size_t b, il::ValueId callee) {
        il::Instruction i;
        i.op = il::Op::DynamicCall;
        i.type = il::Type::Dynamic;
        i.result = next++;
        i.operands = {callee, callee};
        func.blocks[b].instructions.push_back(i);
        return func.blocks[b].instructions.size() - 1;
    }

    // `br %c, then, else` — the guard branch a region ends each of its blocks
    // with, and the edge a chain follows.
    void branch(size_t b, size_t thenBlock, size_t elseBlock) {
        il::Instruction i;
        i.op = il::Op::Branch;
        i.type = il::Type::Void;
        i.operands = {0};
        i.target.block = static_cast<il::BlockId>(thenBlock);
        i.elseTarget.block = static_cast<il::BlockId>(elseBlock);
        func.blocks[b].instructions.push_back(i);
    }

    void jump(size_t b, size_t to) {
        il::Instruction i;
        i.op = il::Op::Jump;
        i.type = il::Type::Void;
        i.target.block = static_cast<il::BlockId>(to);
        func.blocks[b].instructions.push_back(i);
    }

    void ret(size_t b) {
        il::Instruction i;
        i.op = il::Op::Ret;
        i.type = il::Type::Void;
        func.blocks[b].instructions.push_back(i);
    }

    BlockRunPlan planOf(const il::Module& module, size_t b, bool carry = true) {
        func.valueCount = next;
        return planBlockRuns(module, func, b, carry);
    }
};

// The `applyMatrix4` shape: `count` blocks, one read each off `recv`, each
// branching to the next on its guard and to a bail-out block that is NOT part
// of the chain. Answers the read sites, block by block.
struct GuardChain {
    std::vector<size_t> blocks;
    std::vector<size_t> sites;
};

GuardChain buildGuardChain(FuncBuilder& fb, il::ValueId recv, uint32_t count) {
    GuardChain out;
    for (uint32_t k = 0; k < count; ++k) out.blocks.push_back(fb.block());
    const size_t bail = fb.block(/*fast=*/false);
    fb.ret(bail);
    for (uint32_t k = 0; k < count; ++k) {
        out.sites.push_back(fb.read(out.blocks[k], recv, k));
        if (k + 1 < count) {
            fb.branch(out.blocks[k], out.blocks[k + 1], bail);
        } else {
            fb.ret(out.blocks[k]);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("a read run spans the straight line a guarded region splits a block into") {
    const il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();
    const GuardChain chain = buildGuardChain(fb, recv, 8);

    // Every member is in ONE run, the head establishes it, and the single
    // ladder in front of it is asked about the largest index the whole chain
    // reads — not the largest its own block reads.
    for (uint32_t k = 0; k < 8; ++k) {
        const BlockRunPlan plan = fb.planOf(module, chain.blocks[k]);
        const auto site = plan.reads.at(chain.sites[k]);
        INFO("chain member ", k);
        CHECK(site.run == 0u);
        CHECK(site.runMaxIndex == 7u);
        CHECK(site.establishes == (k == 0));
        // What the emitter has to agree with before it keeps a proof across the
        // edge into this block.
        CHECK(plan.continues == (k == 0 ? il::kNoBlock : static_cast<il::BlockId>(chain.blocks[k - 1])));
    }
}

TEST_CASE("BRONZE_NO_SLOT_STORE_CARRY leaves every block planning alone") {
    const il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();
    const GuardChain chain = buildGuardChain(fb, recv, 8);

    // One read to a block is a run of one, and a run of one buys nothing, so
    // with the seam off the planner refuses all eight — which is exactly what
    // it did before chains existed.
    for (uint32_t k = 0; k < 8; ++k) {
        const BlockRunPlan plan = fb.planOf(module, chain.blocks[k], /*carry=*/false);
        INFO("chain member ", k);
        CHECK(plan.reads.at(chain.sites[k]).run == ReceiverRunPlan::kNoRun);
        CHECK(plan.continues == il::kNoBlock);
    }
}

TEST_CASE("a chain stops at a block something else can reach") {
    const il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();

    const size_t b0 = fb.block();
    const size_t b1 = fb.block();
    const size_t b2 = fb.block();
    const size_t joiner = fb.block();
    const size_t bail = fb.block(/*fast=*/false);
    fb.ret(bail);

    const size_t r0 = fb.read(b0, recv, 0);
    fb.branch(b0, b1, bail);
    const size_t r1 = fb.read(b1, recv, 1);
    fb.branch(b1, b2, bail);
    const size_t r2 = fb.read(b2, recv, 2);
    fb.ret(b2);
    // A second edge into b2. It is no longer dominated by b0, so a value
    // defined there does not reach it and the chain has to stop at b1.
    fb.jump(joiner, b2);

    const BlockRunPlan head = fb.planOf(module, b0);
    CHECK(head.reads.at(r0).run == 0u);
    CHECK(head.reads.at(r0).runMaxIndex == 1u);
    CHECK(fb.planOf(module, b1).reads.at(r1).run == 0u);
    // b2 opens its own chain, and one read is a run of one.
    const BlockRunPlan tail = fb.planOf(module, b2);
    CHECK(tail.continues == il::kNoBlock);
    CHECK(tail.reads.at(r2).run == ReceiverRunPlan::kNoRun);
}

TEST_CASE("a chain stops at a block that takes a parameter") {
    const il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();

    const size_t b0 = fb.block();
    const size_t b1 = fb.block();
    const size_t b2 = fb.block();
    const size_t bail = fb.block(/*fast=*/false);
    fb.ret(bail);
    // A block parameter is a phi, and a phi is a value the chain's head does
    // not define — so the domination argument this rests on stops being about
    // everything in the block.
    fb.addParam(b2);

    const size_t r0 = fb.read(b0, recv, 0);
    fb.branch(b0, b1, bail);
    const size_t r1 = fb.read(b1, recv, 1);
    fb.branch(b1, b2, bail);
    const size_t r2 = fb.read(b2, recv, 2);
    fb.ret(b2);

    CHECK(fb.planOf(module, b0).reads.at(r0).runMaxIndex == 1u);
    CHECK(fb.planOf(module, b1).reads.at(r1).run == 0u);
    CHECK(fb.planOf(module, b2).reads.at(r2).run == ReceiverRunPlan::kNoRun);
    CHECK(fb.planOf(module, b2).continues == il::kNoBlock);
}

TEST_CASE("a chain stops at the other copy of the region") {
    const il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();

    const size_t b0 = fb.block();
    const size_t b1 = fb.block(/*fast=*/false);
    const size_t bail = fb.block(/*fast=*/false);
    fb.ret(bail);

    const size_t r0 = fb.read(b0, recv, 0);
    fb.branch(b0, b1, bail);
    const size_t r1 = fb.read(b1, recv, 1);
    fb.ret(b1);

    // The two copies of a region are alternatives, and a `Shared` block is
    // reached from both. Neither is the straight line this is about.
    CHECK(fb.planOf(module, b0).reads.at(r0).run == ReceiverRunPlan::kNoRun);
    CHECK(fb.planOf(module, b1).reads.at(r1).run == ReceiverRunPlan::kNoRun);
    CHECK(fb.planOf(module, b1).continues == il::kNoBlock);
}

TEST_CASE("something that can collect inside a chain member ends the run there") {
    const il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();

    const size_t b0 = fb.block();
    const size_t b1 = fb.block();
    const size_t b2 = fb.block();
    const size_t b3 = fb.block();
    const size_t bail = fb.block(/*fast=*/false);
    fb.ret(bail);

    const size_t r0 = fb.read(b0, recv, 0);
    fb.branch(b0, b1, bail);
    const size_t r1 = fb.read(b1, recv, 1);
    fb.branch(b1, b2, bail);
    // A call in the middle of the chain. The chain still spans b2 — a chain is
    // about domination — but the RUN cannot, because the derived base pointer
    // does not survive a collection.
    fb.call(b2, recv);
    const size_t r2 = fb.read(b2, recv, 2);
    fb.branch(b2, b3, bail);
    const size_t r3 = fb.read(b3, recv, 3);
    fb.ret(b3);

    CHECK(fb.planOf(module, b0).reads.at(r0).run == 0u);
    CHECK(fb.planOf(module, b0).reads.at(r0).runMaxIndex == 1u);
    CHECK(fb.planOf(module, b1).reads.at(r1).run == 0u);
    CHECK(fb.planOf(module, b2).reads.at(r2).run == 1u);
    CHECK(fb.planOf(module, b2).reads.at(r2).establishes);
    CHECK(fb.planOf(module, b3).reads.at(r3).run == 1u);
    CHECK(fb.planOf(module, b3).reads.at(r3).runMaxIndex == 3u);
}

TEST_CASE("a named store between two chain members is spanned, its own receiver is not") {
    const il::Module module = makeModule();

    SUBCASE("another object") {
        FuncBuilder fb;
        const il::ValueId recv = fb.param();
        const il::ValueId other = fb.param();
        const size_t b0 = fb.block();
        const size_t b1 = fb.block();
        const size_t bail = fb.block(/*fast=*/false);
        fb.ret(bail);

        const size_t r0 = fb.read(b0, recv, 0);
        fb.store(b0, other, kNamedKey, recv);
        fb.branch(b0, b1, bail);
        const size_t r1 = fb.read(b1, recv, 1);
        fb.ret(b1);

        CHECK(fb.planOf(module, b0).reads.at(r0).run == 0u);
        CHECK(fb.planOf(module, b0).reads.at(r0).runMaxIndex == 1u);
        CHECK(fb.planOf(module, b1).reads.at(r1).run == 0u);
    }

    SUBCASE("the receiver the run is about") {
        FuncBuilder fb;
        const il::ValueId recv = fb.param();
        const size_t b0 = fb.block();
        const size_t b1 = fb.block();
        const size_t bail = fb.block(/*fast=*/false);
        fb.ret(bail);

        const size_t r0 = fb.read(b0, recv, 0);
        fb.store(b0, recv, kNamedKey, recv);
        fb.branch(b0, b1, bail);
        const size_t r1 = fb.read(b1, recv, 1);
        fb.ret(b1);

        // `arr.foo = 1` on the array the proof is about allocates the side
        // object that holds the name, so this run ends whatever the chain says.
        CHECK(fb.planOf(module, b0).reads.at(r0).run == ReceiverRunPlan::kNoRun);
        CHECK(fb.planOf(module, b1).reads.at(r1).run == ReceiverRunPlan::kNoRun);
    }
}

TEST_CASE("a chain member reached only through an unrelated block plans as its own head") {
    const il::Module module = makeModule();
    FuncBuilder fb;
    const il::ValueId recv = fb.param();

    // Two independent one-read blocks: neither branches to the other, so
    // neither chains and neither run commits. The point is that a chain is
    // built from edges and not from adjacency in the block vector.
    const size_t b0 = fb.block();
    const size_t b1 = fb.block();
    const size_t bail = fb.block(/*fast=*/false);
    fb.ret(bail);

    const size_t r0 = fb.read(b0, recv, 0);
    fb.branch(b0, bail, bail);
    const size_t r1 = fb.read(b1, recv, 1);
    fb.ret(b1);

    CHECK(fb.planOf(module, b0).reads.at(r0).run == ReceiverRunPlan::kNoRun);
    CHECK(fb.planOf(module, b1).reads.at(r1).run == ReceiverRunPlan::kNoRun);
    CHECK(fb.planOf(module, b1).continues == il::kNoBlock);
}
