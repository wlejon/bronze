// WHERE THE HOLE CORRECTION IS PAID (src/codegen-llvm/llvm_recv_proof.h).
//
// A proven element read gives back the element's bits. A hole's bits are an
// internal tag, so somewhere between the load and the program the read owes a
// correction to `undefined` — and the only question is where. At the READ it
// is one select per element, on the straight line a run of sixteen is. At the
// RELOAD it is one select per use that needs it, and the uses of a guarded
// numeric region's reads mostly do not: `is.number` answers false for the hole
// tag and for `undefined` alike, and a RAW `unbox.f64` stands only where
// something already proved the bits are a Number.
//
// So the choice is a count, and `planHoleRawSlots` is that count. Pinned here
// on the PLAN, for the reason run_chain_test.cpp gives: the plan is where the
// decision is taken, and what the emitted ladder does with it is pinned
// against bytes in tests/oracle/cases/array_hole_proven_run.js.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "codegen-llvm/llvm_recv_proof.h"
#include "il/il.h"

using namespace bronze;
using namespace bronze::codegen_llvm;

namespace {

// A function under construction: one block, values numbered as the IL numbers
// them, and a terminator added by whichever test needs an edge.
struct HoleFuncBuilder {
    il::Function func;
    il::ValueId next = 0;

    HoleFuncBuilder() {
        func.name = "f";
        func.returnType = il::Type::Dynamic;
        il::Block b;
        b.id = 0;
        func.blocks.push_back(b);
        il::Block slow;
        slow.id = 1;
        func.blocks.push_back(slow);
    }

    il::ValueId param() { return next++; }

    il::ValueId read(il::ValueId recv, uint32_t keyIndex) {
        il::Instruction i;
        i.op = il::Op::PropGet;
        i.type = il::Type::Dynamic;
        i.result = next++;
        i.operands = {recv};
        i.keyIndex = keyIndex;
        func.blocks[0].instructions.push_back(i);
        return i.result;
    }

    il::ValueId guard(il::ValueId v) {
        il::Instruction i;
        i.op = il::Op::IsNumber;
        i.type = il::Type::Bool;
        i.result = next++;
        i.operands = {v};
        func.blocks[0].instructions.push_back(i);
        return i.result;
    }

    il::ValueId rawUnbox(il::ValueId v) {
        il::Instruction i;
        i.op = il::Op::Unbox;
        i.type = il::Type::F64;
        i.rawUnbox = true;
        i.result = next++;
        i.operands = {v};
        func.blocks[0].instructions.push_back(i);
        return i.result;
    }

    // The checked coercion, which is NOT hole-insensitive: it runs ToNumber
    // over whatever the value is, and `undefined` and a hole would take
    // different roads through it if the bits ever reached it uncorrected.
    il::ValueId checkedUnbox(il::ValueId v) {
        il::Instruction i;
        i.op = il::Op::Unbox;
        i.type = il::Type::F64;
        i.result = next++;
        i.operands = {v};
        func.blocks[0].instructions.push_back(i);
        return i.result;
    }

    void store(il::ValueId recv, uint32_t keyIndex, il::ValueId value) {
        il::Instruction i;
        i.op = il::Op::PropSet;
        i.type = il::Type::Void;
        i.operands = {recv, value};
        i.keyIndex = keyIndex;
        func.blocks[0].instructions.push_back(i);
    }

    // The trampoline out of a fast copy: one edge carrying the boxed values
    // the slow copy is entered with.
    void bail(const std::vector<il::ValueId>& args) {
        il::Instruction i;
        i.op = il::Op::Jump;
        i.type = il::Type::Void;
        i.target.block = 1;
        i.target.args = args;
        func.blocks[0].instructions.push_back(i);
    }

    std::vector<uint8_t> plan() {
        func.valueCount = next;
        return planHoleRawSlots(func);
    }
};

}  // namespace

TEST_CASE("a read consumed by a guard and a raw unbox takes the hole-raw slot") {
    // `multiplyMatrices` in one element: read, guard, raw unbox behind the
    // guard, and the one edge into the slow copy. Two insensitive uses against
    // one that is not, so moving the correction off the read removes one select
    // and adds one, on an edge that is taken at most once per call.
    HoleFuncBuilder b;
    const il::ValueId recv = b.param();
    const il::ValueId v = b.read(recv, 0);
    b.guard(v);
    b.rawUnbox(v);
    b.bail({v});
    CHECK(b.plan()[v] == 1);
}

TEST_CASE("a read that goes straight into a store keeps the correction at the read") {
    // `Matrix4.copy`: the value is boxed all the way to a `prop.set`, so a
    // hole-raw slot would pay the same select one block later and nothing
    // would have been saved.
    HoleFuncBuilder b;
    const il::ValueId src = b.param();
    const il::ValueId dst = b.param();
    const il::ValueId v = b.read(src, 0);
    b.store(dst, 0, v);
    CHECK(b.plan()[v] == 0);
}

TEST_CASE("a CHECKED unbox is a use that needs the correction") {
    // Only the RAW unbox carries the claim that the bits are a Number. The
    // checked one is ToNumber over an unproven value and must see `undefined`.
    HoleFuncBuilder b;
    const il::ValueId recv = b.param();
    const il::ValueId v = b.read(recv, 0);
    b.checkedUnbox(v);
    CHECK(b.plan()[v] == 0);
}

TEST_CASE("a guard alone is enough, and a value with no insensitive use is not") {
    HoleFuncBuilder b;
    const il::ValueId recv = b.param();
    const il::ValueId guarded = b.read(recv, 0);
    const il::ValueId plain = b.read(recv, 1);
    b.guard(guarded);
    b.bail({plain});
    const std::vector<uint8_t> pays = b.plan();
    CHECK(pays[guarded] == 1);
    CHECK(pays[plain] == 0);
}

TEST_CASE("the receiver of a read is not an insensitive use of itself") {
    // `holeInsensitiveUse` reads operand 0 and nothing else, so a value that
    // happens to be a guard's operand elsewhere is not credited for standing
    // in some other instruction's first slot.
    HoleFuncBuilder b;
    const il::ValueId recv = b.param();
    b.read(recv, 0);
    b.read(recv, 1);
    CHECK(b.plan()[recv] == 0);
}

TEST_CASE("holeInsensitiveUse names exactly the two ops that may read raw bits") {
    il::Instruction isNum;
    isNum.op = il::Op::IsNumber;
    isNum.operands = {7};
    CHECK(holeInsensitiveUse(isNum, 7));
    CHECK_FALSE(holeInsensitiveUse(isNum, 8));

    il::Instruction raw;
    raw.op = il::Op::Unbox;
    raw.type = il::Type::F64;
    raw.rawUnbox = true;
    raw.operands = {7};
    CHECK(holeInsensitiveUse(raw, 7));

    il::Instruction checked = raw;
    checked.rawUnbox = false;
    CHECK_FALSE(holeInsensitiveUse(checked, 7));

    il::Instruction rawI32 = raw;
    rawI32.type = il::Type::I32;
    CHECK_FALSE(holeInsensitiveUse(rawI32, 7));
}
