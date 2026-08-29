// What the ARRAY STORE-RUN planner will and will not prove
// (src/codegen-llvm/llvm_array_store_proof.h), and what happens when a run of
// constant-index reads and a run of constant-index stores are threaded through
// each other — the `Matrix4.copy` shape, where neither run is a contiguous
// stretch of instructions and the store used to kill the read at its first
// member.
//
// Pinned on the PLAN rather than on emitted IR, for the reason
// store_proof_test.cpp gives: the plan is where every decision this file is
// about is taken, and a plan is a value a test can read without standing an
// LLVM module up around it. What the LADDER does with a plan is pinned in
// tests/oracle/cases/recv_proof_array_stores.js, against bytes.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "codegen-llvm/llvm_array_store_proof.h"
#include "codegen-llvm/llvm_recv_proof.h"
#include "il/il.h"

using namespace bronze;
using namespace bronze::codegen_llvm;

namespace {

// The key table every test here shares: sixteen index keys at indices 0..15,
// then two names, so a test can spell `te[3]` and `te.foo` in the same block.
constexpr uint32_t kNamedKey = 16;   // "foo"
constexpr uint32_t kLengthKey = 17;  // "length"

il::Module makeModule() {
    il::Module module;
    module.name = "array_store_proof_test";
    for (uint32_t i = 0; i < 16; ++i) module.keyConstants.push_back(std::to_string(i));
    module.keyConstants.push_back("foo");
    module.keyConstants.push_back("length");
    return module;
}

// One block under construction, with the value numbering the IL's SSA wants.
struct BlockBuilder {
    il::Block block;
    il::ValueId next = 0;

    il::ValueId param() { return next++; }

    // `recv[k] = value` — a PropSet against a constant key, which is what the
    // front end lowers a constant-index element write to.
    size_t store(il::ValueId recv, uint32_t keyIndex, il::ValueId value) {
        il::Instruction i;
        i.op = il::Op::PropSet;
        i.type = il::Type::Void;
        i.result = il::kNoValue;
        i.operands = {recv, value};
        i.keyIndex = keyIndex;
        block.instructions.push_back(i);
        return block.instructions.size() - 1;
    }

    // `recv[k]`, which is a PropGet against a constant index key.
    size_t read(il::ValueId recv, uint32_t keyIndex, il::ValueId& out) {
        il::Instruction i;
        i.op = il::Op::PropGet;
        i.type = il::Type::Dynamic;
        i.result = next++;
        i.operands = {recv};
        i.keyIndex = keyIndex;
        block.instructions.push_back(i);
        out = i.result;
        return block.instructions.size() - 1;
    }

    // A call: the archetypal `il::canCollect`, and what ends every run.
    size_t call(il::ValueId callee, il::ValueId thisVal) {
        il::Instruction i;
        i.op = il::Op::DynamicCall;
        i.type = il::Type::Dynamic;
        i.result = next++;
        i.operands = {callee, thisVal};
        block.instructions.push_back(i);
        return block.instructions.size() - 1;
    }

    // An instruction that cannot collect but DEFINES a value — what a test
    // needs to redefine a receiver without also ending the run for the other
    // reason.
    il::ValueId redefine(il::ValueId id) {
        il::Instruction i;
        i.op = il::Op::ConstF64;
        i.type = il::Type::F64;
        i.result = id;
        i.immF64 = 0.0;
        block.instructions.push_back(i);
        return id;
    }
};

BlockRunPlan planOf(const il::Module& module, BlockBuilder& b) {
    il::Function func;
    func.name = "f";
    func.returnType = il::Type::Dynamic;
    func.valueCount = b.next;
    func.blocks = {b.block};
    return planBlockRuns(module, func, 0);
}

}  // namespace

TEST_CASE("the array-store planner recognises a run of constant-index writes") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();
    const il::ValueId value = b.param();

    std::vector<size_t> at;
    for (uint32_t k = 0; k < 4; ++k) at.push_back(b.store(target, k, value));

    const BlockRunPlan plan = planOf(module, b);
    for (size_t i = 0; i < at.size(); ++i) {
        const ArrayStoreRunPlan::Site site = plan.arrayStores.at(at[i]);
        CHECK(site.run == 0u);
        CHECK(site.establishes == (i == 0));
        // ONE bound for the whole run, and it is the largest index in it —
        // which is what the single length test and the single capacity test
        // have to clear for every member at once.
        CHECK(site.runMaxIndex == 3u);
        CHECK(site.index == static_cast<uint32_t>(i));
    }
}

TEST_CASE("the run's bound is its LARGEST index, whatever order the members come in") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();
    const il::ValueId value = b.param();

    // Descending, and then one in the middle: the bound is 9 for all of them.
    const size_t s0 = b.store(target, 9, value);
    const size_t s1 = b.store(target, 4, value);
    const size_t s2 = b.store(target, 6, value);

    const BlockRunPlan plan = planOf(module, b);
    for (size_t at : {s0, s1, s2}) CHECK(plan.arrayStores.at(at).runMaxIndex == 9u);
    CHECK(plan.arrayStores.at(s0).index == 9u);
    CHECK(plan.arrayStores.at(s1).index == 4u);
    CHECK(plan.arrayStores.at(s2).index == 6u);
}

TEST_CASE("a store run at index 5 into a length-3 array is PLANNED and fails at run time") {
    // The planner cannot know how long the array is: it is a fact about the
    // heap, and the one length test in the ladder is where it is asked. What
    // the plan owes is that the test asked about is the run's LARGEST index, so
    // a run that would write past the end is refused whole rather than
    // half-performed. (The behaviour on the other side of that refusal —
    // `arr[5] = v` on a length-3 array setting `length` to 6 — is pinned in the
    // oracle case, because it is a fact about the language.)
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();
    const il::ValueId value = b.param();
    const size_t s0 = b.store(target, 0, value);
    const size_t s1 = b.store(target, 5, value);

    const BlockRunPlan plan = planOf(module, b);
    CHECK(plan.arrayStores.at(s0).run == 0u);
    CHECK(plan.arrayStores.at(s1).run == 0u);
    CHECK(plan.arrayStores.at(s0).runMaxIndex == 5u);
    CHECK(plan.arrayStores.at(s1).runMaxIndex == 5u);
}

TEST_CASE("an array-store run stops at the first instruction that can collect") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();
    const il::ValueId value = b.param();

    const size_t s0 = b.store(target, 0, value);
    const size_t s1 = b.store(target, 1, value);
    b.call(value, value);
    const size_t s2 = b.store(target, 2, value);
    const size_t s3 = b.store(target, 3, value);

    const BlockRunPlan plan = planOf(module, b);
    CHECK(plan.arrayStores.at(s0).run == 0u);
    CHECK(plan.arrayStores.at(s1).run == 0u);
    CHECK(plan.arrayStores.at(s0).runMaxIndex == 1u);
    // A second run, with a bound of its own: the call could have moved the
    // elements block, so nothing the first run proved reaches the third store.
    CHECK(plan.arrayStores.at(s2).run == 1u);
    CHECK(plan.arrayStores.at(s2).establishes);
    CHECK(plan.arrayStores.at(s3).run == 1u);
    CHECK(plan.arrayStores.at(s2).runMaxIndex == 3u);
}

TEST_CASE("an array-store run stops when its receiver is redefined") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();
    const il::ValueId value = b.param();
    const size_t s0 = b.store(target, 0, value);
    const size_t s1 = b.store(target, 1, value);
    b.redefine(target);
    const size_t s2 = b.store(target, 2, value);
    const size_t s3 = b.store(target, 3, value);

    const BlockRunPlan plan = planOf(module, b);
    CHECK(plan.arrayStores.at(s0).run == 0u);
    CHECK(plan.arrayStores.at(s1).run == 0u);
    CHECK(plan.arrayStores.at(s2).run == 1u);
    CHECK(plan.arrayStores.at(s3).run == 1u);
}

TEST_CASE("a store whose key is not an index is in no run, and ends the one over it") {
    il::Module module = makeModule();

    SUBCASE("a name") {
        BlockBuilder b;
        const il::ValueId target = b.param();
        const il::ValueId value = b.param();
        const size_t s0 = b.store(target, 0, value);
        const size_t s1 = b.store(target, 1, value);
        const size_t named = b.store(target, kNamedKey, value);
        const size_t s2 = b.store(target, 2, value);
        const size_t s3 = b.store(target, 3, value);

        const BlockRunPlan plan = planOf(module, b);
        CHECK(plan.arrayStores.at(named).run == ArrayStoreRunPlan::kNoRun);
        CHECK(plan.arrayStores.at(s0).run == 0u);
        CHECK(plan.arrayStores.at(s1).run == 0u);
        // `arr.foo = 1` reaches bronze_prop_set, which allocates the side
        // object — so the proof cannot cross it and the run starts again.
        CHECK(plan.arrayStores.at(s2).run == 1u);
        CHECK(plan.arrayStores.at(s3).run == 1u);
    }

    SUBCASE("`length`") {
        BlockBuilder b;
        const il::ValueId target = b.param();
        const il::ValueId value = b.param();
        const size_t s0 = b.store(target, 0, value);
        const size_t s1 = b.store(target, 1, value);
        const size_t len = b.store(target, kLengthKey, value);

        const BlockRunPlan plan = planOf(module, b);
        CHECK(plan.arrayStores.at(len).run == ArrayStoreRunPlan::kNoRun);
        CHECK(plan.arrayStores.at(s0).run == 0u);
        CHECK(plan.arrayStores.at(s1).run == 0u);
    }
}

TEST_CASE("an array-store run is about one receiver") {
    il::Module module = makeModule();

    SUBCASE("a second receiver opens a second run") {
        BlockBuilder b;
        const il::ValueId a = b.param();
        const il::ValueId c = b.param();
        const il::ValueId value = b.param();
        const size_t s0 = b.store(a, 0, value);
        const size_t s1 = b.store(a, 1, value);
        const size_t s2 = b.store(c, 2, value);
        const size_t s3 = b.store(c, 3, value);

        const BlockRunPlan plan = planOf(module, b);
        CHECK(plan.arrayStores.at(s0).run == 0u);
        CHECK(plan.arrayStores.at(s1).run == 0u);
        CHECK(plan.arrayStores.at(s0).runMaxIndex == 1u);
        CHECK(plan.arrayStores.at(s2).run == 1u);
        CHECK(plan.arrayStores.at(s3).run == 1u);
        CHECK(plan.arrayStores.at(s2).runMaxIndex == 3u);
    }

    SUBCASE("a run of one is not worth a proof") {
        BlockBuilder b;
        const il::ValueId target = b.param();
        const il::ValueId value = b.param();
        const size_t only = b.store(target, 0, value);
        b.call(value, value);

        const BlockRunPlan plan = planOf(module, b);
        CHECK(plan.arrayStores.at(only).run == ArrayStoreRunPlan::kNoRun);
    }
}

TEST_CASE("the Matrix4.copy shape: a read run and an array-store run both survive") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();  // this.elements
    const il::ValueId source = b.param();  // m.elements

    // `te[k] = me[k]`, sixteen times over. Neither run is contiguous, and each
    // member of one sits inside the other's span. Before this planner the FIRST
    // store ended the read run — `prop.set` is `il::canCollect` — and every
    // read paid its own ladder.
    std::vector<size_t> reads;
    std::vector<size_t> stores;
    for (uint32_t k = 0; k < 16; ++k) {
        il::ValueId v = il::kNoValue;
        reads.push_back(b.read(source, k, v));
        stores.push_back(b.store(target, k, v));
    }

    const BlockRunPlan plan = planOf(module, b);
    for (size_t k = 0; k < 16; ++k) {
        CHECK(plan.reads.at(reads[k]).run == 0u);
        CHECK(plan.reads.at(reads[k]).runMaxIndex == 15u);
        CHECK(plan.reads.at(reads[k]).establishes == (k == 0));
        CHECK(plan.arrayStores.at(stores[k]).run == 0u);
        CHECK(plan.arrayStores.at(stores[k]).runMaxIndex == 15u);
        CHECK(plan.arrayStores.at(stores[k]).establishes == (k == 0));
    }
}

TEST_CASE("the read-modify-write shape: both runs on ONE receiver") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId te = b.param();

    // `te[0] = te[1]; te[1] = te[0];` — one array, read and written. The two
    // proofs are independent and both derive the same base; what keeps the
    // second read from being hoisted over the first store is the alias family
    // they share, not the plan, and the ORDER is pinned in the oracle case.
    il::ValueId v0 = il::kNoValue;
    const size_t r0 = b.read(te, 1, v0);
    const size_t w0 = b.store(te, 0, v0);
    il::ValueId v1 = il::kNoValue;
    const size_t r1 = b.read(te, 0, v1);
    const size_t w1 = b.store(te, 1, v1);

    const BlockRunPlan plan = planOf(module, b);
    CHECK(plan.reads.at(r0).run == 0u);
    CHECK(plan.reads.at(r1).run == 0u);
    CHECK(plan.reads.at(r0).runMaxIndex == 1u);
    CHECK(plan.arrayStores.at(w0).run == 0u);
    CHECK(plan.arrayStores.at(w1).run == 0u);
    CHECK(plan.arrayStores.at(w0).runMaxIndex == 1u);
}

TEST_CASE("an array store the planner refuses ends the read run standing over it") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();
    const il::ValueId source = b.param();

    // Four reads with one NAMED store in the middle. It never becomes a run
    // member, and a `prop.set` that is not a member is an ordinary
    // `canCollect` instruction — it reaches bronze_prop_set, which can run a
    // setter. This is what EVERY constant-index store did to a read run before
    // this planner existed.
    std::vector<size_t> reads;
    il::ValueId v = il::kNoValue;
    reads.push_back(b.read(source, 0, v));
    reads.push_back(b.read(source, 1, v));
    b.store(target, kNamedKey, v);
    reads.push_back(b.read(source, 2, v));
    reads.push_back(b.read(source, 3, v));

    const BlockRunPlan plan = planOf(module, b);
    CHECK(plan.reads.at(reads[0]).run == 0u);
    CHECK(plan.reads.at(reads[1]).run == 0u);
    CHECK(plan.reads.at(reads[0]).runMaxIndex == 1u);
    CHECK(plan.reads.at(reads[2]).run == 1u);
    CHECK(plan.reads.at(reads[3]).run == 1u);
    for (const auto& site : plan.arrayStores.sites) {
        CHECK(site.run == ArrayStoreRunPlan::kNoRun);
    }
}

TEST_CASE("an array-store run that fails to commit becomes opaque, and the plan settles again") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();
    const il::ValueId source = b.param();

    // The fixpoint the joint planner exists for, now with three run kinds in
    // it. Optimistically the lone store is a run member, so all four reads are
    // one run; but an array-store run of ONE buys nothing and is dropped, which
    // makes the store opaque again — and the second pass has to end the read
    // run at it rather than leave a plan that says a proof crosses an
    // instruction nothing carries it across.
    std::vector<size_t> reads;
    il::ValueId v = il::kNoValue;
    reads.push_back(b.read(source, 0, v));
    b.store(target, 0, v);
    reads.push_back(b.read(source, 1, v));
    reads.push_back(b.read(source, 2, v));
    reads.push_back(b.read(source, 3, v));

    const BlockRunPlan plan = planOf(module, b);
    CHECK(plan.reads.at(reads[0]).run == ReceiverRunPlan::kNoRun);
    CHECK(plan.reads.at(reads[1]).run != ReceiverRunPlan::kNoRun);
    CHECK(plan.reads.at(reads[1]).run == plan.reads.at(reads[2]).run);
    CHECK(plan.reads.at(reads[1]).run == plan.reads.at(reads[3]).run);
    CHECK(plan.reads.at(reads[1]).establishes);
    CHECK(plan.reads.at(reads[1]).runMaxIndex == 3u);
    for (const auto& site : plan.arrayStores.sites) {
        CHECK(site.run == ArrayStoreRunPlan::kNoRun);
    }
}
