// What the STORE-RUN planner will and will not prove
// (src/codegen-llvm/llvm_store_proof.h), and what happens when a run of reads
// and a run of stores are threaded through each other — the `Matrix4.toArray`
// shape, where neither run is a contiguous stretch of instructions.
//
// Pinned on the PLAN rather than on emitted IR: the plan is where every
// decision this file is about is taken, and a plan is a value a test can read
// without standing an LLVM module up around it.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "codegen-llvm/llvm_recv_proof.h"
#include "codegen-llvm/llvm_store_proof.h"
#include "il/il.h"

using namespace bronze;
using namespace bronze::codegen_llvm;

namespace {

// One block under construction, with the value numbering the IL's SSA wants.
// Every helper returns the ValueId it defined, so a test reads as the program
// it is about.
struct BlockBuilder {
    il::Block block;
    il::ValueId next = 0;

    il::ValueId param() { return next++; }

    il::ValueId constF64(double v) {
        il::Instruction i;
        i.op = il::Op::ConstF64;
        i.type = il::Type::F64;
        i.result = next++;
        i.immF64 = v;
        block.instructions.push_back(i);
        return i.result;
    }

    il::ValueId add(il::ValueId a, il::ValueId b) {
        il::Instruction i;
        i.op = il::Op::Add;
        i.type = il::Type::F64;
        i.result = next++;
        i.operands = {a, b};
        block.instructions.push_back(i);
        return i.result;
    }

    il::ValueId boxF64(il::ValueId v) {
        il::Instruction i;
        i.op = il::Op::Box;
        i.type = il::Type::Dynamic;
        i.result = next++;
        i.operands = {v};
        i.boxType = il::Type::F64;
        block.instructions.push_back(i);
        return i.result;
    }

    // `recv[base + k] = value`, spelled the way the guarded-region pass spells
    // it: the offset boxed off an add, or the base boxed bare for k == 0.
    size_t store(il::ValueId recv, il::ValueId base, uint32_t k, il::ValueId value) {
        const il::ValueId index = (k == 0) ? boxF64(base) : boxF64(add(base, constF64(k)));
        il::Instruction i;
        i.op = il::Op::ElemSet;
        i.type = il::Type::Void;
        i.result = il::kNoValue;
        i.operands = {recv, index, value};
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
    // needs to redefine a receiver or a base without also ending the run for
    // the other reason.
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

// The module every test here shares: sixteen index keys, which is all the read
// planner needs to recognise `te[0]`..`te[15]`.
il::Module makeModule() {
    il::Module module;
    module.name = "store_proof_test";
    for (uint32_t i = 0; i < 16; ++i) module.keyConstants.push_back(std::to_string(i));
    return module;
}

BlockRunPlan planOf(const il::Module& module, BlockBuilder& b) {
    il::Function func;
    func.name = "f";
    func.returnType = il::Type::Dynamic;
    func.valueCount = b.next;
    func.blocks = {b.block};
    return planBlockRuns(module, func, 0);
}

}  // namespace

TEST_CASE("the store planner recognises a run of affine writes off one base") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();
    const il::ValueId base = b.param();
    const il::ValueId value = b.param();

    std::vector<size_t> at;
    for (uint32_t k = 0; k < 4; ++k) at.push_back(b.store(target, base, k, value));

    const BlockRunPlan plan = planOf(module, b);
    for (size_t i = 0; i < at.size(); ++i) {
        const StoreRunPlan::Site site = plan.stores.at(at[i]);
        CHECK(site.run == 0u);
        CHECK(site.establishes == (i == 0));
        // ONE bound for the whole run, and it is the largest offset in it.
        CHECK(site.runMaxOffset == 3u);
        CHECK(site.offset == static_cast<uint32_t>(i));
        CHECK(site.base == base);
    }
}

TEST_CASE("a store run accepts the constant on either side of the add") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();
    const il::ValueId base = b.param();
    const il::ValueId value = b.param();

    // `k + base` rather than `base + k`: the same index, spelled the other way.
    const size_t first = [&] {
        const il::ValueId idx = b.boxF64(b.add(b.constF64(5), base));
        il::Instruction i;
        i.op = il::Op::ElemSet;
        i.operands = {target, idx, value};
        b.block.instructions.push_back(i);
        return b.block.instructions.size() - 1;
    }();
    const size_t second = b.store(target, base, 6, value);

    const BlockRunPlan plan = planOf(module, b);
    CHECK(plan.stores.at(first).run == 0u);
    CHECK(plan.stores.at(first).offset == 5u);
    CHECK(plan.stores.at(second).run == 0u);
    CHECK(plan.stores.at(second).offset == 6u);
    CHECK(plan.stores.at(second).runMaxOffset == 6u);
}

TEST_CASE("a store run stops at the first instruction that can collect") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();
    const il::ValueId base = b.param();
    const il::ValueId value = b.param();

    const size_t s0 = b.store(target, base, 0, value);
    const size_t s1 = b.store(target, base, 1, value);
    b.call(value, value);
    const size_t s2 = b.store(target, base, 2, value);
    const size_t s3 = b.store(target, base, 3, value);

    const BlockRunPlan plan = planOf(module, b);
    CHECK(plan.stores.at(s0).run == 0u);
    CHECK(plan.stores.at(s1).run == 0u);
    CHECK(plan.stores.at(s0).runMaxOffset == 1u);
    // A second run, with a bound of its own: the call could have moved the
    // buffer, so nothing the first run proved reaches the third store.
    CHECK(plan.stores.at(s2).run == 1u);
    CHECK(plan.stores.at(s2).establishes);
    CHECK(plan.stores.at(s3).run == 1u);
    CHECK(plan.stores.at(s2).runMaxOffset == 3u);
}

TEST_CASE("a store run stops when its receiver or its base is redefined") {
    il::Module module = makeModule();
    SUBCASE("receiver") {
        BlockBuilder b;
        const il::ValueId target = b.param();
        const il::ValueId base = b.param();
        const il::ValueId value = b.param();
        const size_t s0 = b.store(target, base, 0, value);
        const size_t s1 = b.store(target, base, 1, value);
        b.redefine(target);
        const size_t s2 = b.store(target, base, 2, value);
        const size_t s3 = b.store(target, base, 3, value);

        const BlockRunPlan plan = planOf(module, b);
        CHECK(plan.stores.at(s0).run == 0u);
        CHECK(plan.stores.at(s1).run == 0u);
        CHECK(plan.stores.at(s2).run == 1u);
        CHECK(plan.stores.at(s3).run == 1u);
    }
    SUBCASE("base") {
        BlockBuilder b;
        const il::ValueId target = b.param();
        const il::ValueId base = b.param();
        const il::ValueId value = b.param();
        const size_t s0 = b.store(target, base, 0, value);
        const size_t s1 = b.store(target, base, 1, value);
        b.redefine(base);
        const size_t s2 = b.store(target, base, 2, value);
        const size_t s3 = b.store(target, base, 3, value);

        const BlockRunPlan plan = planOf(module, b);
        CHECK(plan.stores.at(s0).run == 0u);
        CHECK(plan.stores.at(s1).run == 0u);
        CHECK(plan.stores.at(s2).run == 1u);
        CHECK(plan.stores.at(s3).run == 1u);
    }
}

TEST_CASE("a store the planner cannot read the affine form of is in no run") {
    il::Module module = makeModule();

    SUBCASE("two non-constant operands is not affine") {
        BlockBuilder b;
        const il::ValueId target = b.param();
        const il::ValueId base = b.param();
        const il::ValueId other = b.param();
        const il::ValueId value = b.param();
        const il::ValueId idx = b.boxF64(b.add(base, other));
        il::Instruction i;
        i.op = il::Op::ElemSet;
        i.operands = {target, idx, value};
        b.block.instructions.push_back(i);
        const size_t bad = b.block.instructions.size() - 1;
        const size_t good = b.store(target, base, 1, value);

        const BlockRunPlan plan = planOf(module, b);
        // The unreadable store is in no run, and it can collect, so it also
        // refuses to be the run the store after it would have joined.
        CHECK(plan.stores.at(bad).run == StoreRunPlan::kNoRun);
        CHECK(plan.stores.at(good).run == StoreRunPlan::kNoRun);
    }

    SUBCASE("a negative offset") {
        BlockBuilder b;
        const il::ValueId target = b.param();
        const il::ValueId base = b.param();
        const il::ValueId value = b.param();
        const il::ValueId idx = b.boxF64(b.add(base, b.constF64(-1.0)));
        il::Instruction i;
        i.op = il::Op::ElemSet;
        i.operands = {target, idx, value};
        b.block.instructions.push_back(i);
        const size_t bad = b.block.instructions.size() - 1;
        b.store(target, base, 1, value);

        const BlockRunPlan plan = planOf(module, b);
        CHECK(plan.stores.at(bad).run == StoreRunPlan::kNoRun);
    }

    SUBCASE("a non-integral offset") {
        BlockBuilder b;
        const il::ValueId target = b.param();
        const il::ValueId base = b.param();
        const il::ValueId value = b.param();
        const il::ValueId idx = b.boxF64(b.add(base, b.constF64(1.5)));
        il::Instruction i;
        i.op = il::Op::ElemSet;
        i.operands = {target, idx, value};
        b.block.instructions.push_back(i);
        const size_t bad = b.block.instructions.size() - 1;
        b.store(target, base, 1, value);

        const BlockRunPlan plan = planOf(module, b);
        CHECK(plan.stores.at(bad).run == StoreRunPlan::kNoRun);
    }

    SUBCASE("an offset past the planner's window") {
        BlockBuilder b;
        const il::ValueId target = b.param();
        const il::ValueId base = b.param();
        const il::ValueId value = b.param();
        const il::ValueId idx =
            b.boxF64(b.add(base, b.constF64(static_cast<double>(kMaxStoreOffset) + 1.0)));
        il::Instruction i;
        i.op = il::Op::ElemSet;
        i.operands = {target, idx, value};
        b.block.instructions.push_back(i);
        const size_t bad = b.block.instructions.size() - 1;
        b.store(target, base, 1, value);

        const BlockRunPlan plan = planOf(module, b);
        CHECK(plan.stores.at(bad).run == StoreRunPlan::kNoRun);
    }
}

TEST_CASE("a store run is about one receiver and one base") {
    il::Module module = makeModule();

    SUBCASE("a second receiver opens a second run") {
        BlockBuilder b;
        const il::ValueId a = b.param();
        const il::ValueId c = b.param();
        const il::ValueId base = b.param();
        const il::ValueId value = b.param();
        const size_t s0 = b.store(a, base, 0, value);
        const size_t s1 = b.store(a, base, 1, value);
        const size_t s2 = b.store(c, base, 2, value);
        const size_t s3 = b.store(c, base, 3, value);

        const BlockRunPlan plan = planOf(module, b);
        CHECK(plan.stores.at(s0).run == 0u);
        CHECK(plan.stores.at(s1).run == 0u);
        CHECK(plan.stores.at(s0).runMaxOffset == 1u);
        CHECK(plan.stores.at(s2).run == 1u);
        CHECK(plan.stores.at(s3).run == 1u);
        CHECK(plan.stores.at(s2).runMaxOffset == 3u);
    }

    SUBCASE("a second base opens a second run") {
        BlockBuilder b;
        const il::ValueId target = b.param();
        const il::ValueId base = b.param();
        const il::ValueId other = b.param();
        const il::ValueId value = b.param();
        const size_t s0 = b.store(target, base, 0, value);
        const size_t s1 = b.store(target, base, 1, value);
        const size_t s2 = b.store(target, other, 2, value);
        const size_t s3 = b.store(target, other, 3, value);

        const BlockRunPlan plan = planOf(module, b);
        CHECK(plan.stores.at(s0).run == 0u);
        CHECK(plan.stores.at(s1).run == 0u);
        CHECK(plan.stores.at(s2).run == 1u);
        CHECK(plan.stores.at(s3).run == 1u);
        CHECK(plan.stores.at(s2).base == other);
    }

    SUBCASE("a run of one is not worth a proof") {
        BlockBuilder b;
        const il::ValueId target = b.param();
        const il::ValueId base = b.param();
        const il::ValueId value = b.param();
        const size_t only = b.store(target, base, 0, value);
        b.call(value, value);

        const BlockRunPlan plan = planOf(module, b);
        CHECK(plan.stores.at(only).run == StoreRunPlan::kNoRun);
    }
}

TEST_CASE("a read run and a store run threaded through each other both survive") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();  // the Float32Array
    const il::ValueId source = b.param();  // this.elements
    const il::ValueId base = b.param();    // the offset

    // The `Matrix4.toArray` shape: read te[k], store array[base + k], sixteen
    // times over. Neither run is contiguous, and each member of one sits
    // inside the other's span.
    std::vector<size_t> reads;
    std::vector<size_t> stores;
    for (uint32_t k = 0; k < 16; ++k) {
        il::ValueId v = il::kNoValue;
        reads.push_back(b.read(source, k, v));
        stores.push_back(b.store(target, base, k, v));
    }

    const BlockRunPlan plan = planOf(module, b);
    for (size_t k = 0; k < 16; ++k) {
        CHECK(plan.reads.at(reads[k]).run == 0u);
        CHECK(plan.reads.at(reads[k]).runMaxIndex == 15u);
        CHECK(plan.reads.at(reads[k]).establishes == (k == 0));
        CHECK(plan.stores.at(stores[k]).run == 0u);
        CHECK(plan.stores.at(stores[k]).runMaxOffset == 15u);
        CHECK(plan.stores.at(stores[k]).establishes == (k == 0));
    }
}

TEST_CASE("an ElemSet the planner refuses ends the read run standing over it") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();
    const il::ValueId source = b.param();
    const il::ValueId base = b.param();
    const il::ValueId other = b.param();

    // Four reads with one store in the middle whose index is not affine. It
    // never becomes a run member, and an ElemSet that is not a member is an
    // ordinary `canCollect` instruction — it reaches bronze_elem_set, which can
    // run a setter. This is what EVERY elem.set did to a read run before the
    // store proof existed.
    std::vector<size_t> reads;
    il::ValueId v = il::kNoValue;
    reads.push_back(b.read(source, 0, v));
    reads.push_back(b.read(source, 1, v));
    {
        const il::ValueId idx = b.boxF64(b.add(base, other));
        il::Instruction i;
        i.op = il::Op::ElemSet;
        i.operands = {target, idx, v};
        b.block.instructions.push_back(i);
    }
    reads.push_back(b.read(source, 2, v));
    reads.push_back(b.read(source, 3, v));

    const BlockRunPlan plan = planOf(module, b);
    CHECK(plan.reads.at(reads[0]).run == 0u);
    CHECK(plan.reads.at(reads[1]).run == 0u);
    CHECK(plan.reads.at(reads[0]).runMaxIndex == 1u);
    CHECK(plan.reads.at(reads[2]).run == 1u);
    CHECK(plan.reads.at(reads[3]).run == 1u);
    for (const auto& site : plan.stores.sites) CHECK(site.run == StoreRunPlan::kNoRun);
}

TEST_CASE("a store run that fails to commit becomes opaque, and the plan settles again") {
    il::Module module = makeModule();
    BlockBuilder b;
    const il::ValueId target = b.param();
    const il::ValueId source = b.param();
    const il::ValueId base = b.param();

    // The fixpoint the joint planner exists for. Optimistically the store is a
    // run member, so all four reads are one run; but a store run of ONE buys
    // nothing and is dropped, which makes the store opaque again — and the
    // second pass has to end the read run at it rather than leave a plan that
    // says a proof crosses an instruction nothing carries it across.
    std::vector<size_t> reads;
    il::ValueId v = il::kNoValue;
    reads.push_back(b.read(source, 0, v));
    b.store(target, base, 0, v);
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
    for (const auto& site : plan.stores.sites) CHECK(site.run == StoreRunPlan::kNoRun);
}
