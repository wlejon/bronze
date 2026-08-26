// Stage R2: what the representation plan says, and what the store sites emit
// once they have been told.
//
// Two halves, tested two ways on purpose.
//
// The PLAN is a pure function of the IL (llvm_repr.h), so it is checked against
// hand-built IL: an answer read off eight instructions is exact, where the same
// answer read off a compiled fixture is arithmetic on a count.
//
// The SITES are checked by calling the emitter and reading the IR back. What
// has to hold there is a statement about the SHAPE of what was emitted -- how
// many loads a store performs, whether a `sitofp` is in it, whether the
// representation branch exists at all -- and after the O3 pipeline none of
// those questions has an answer any more.

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_abi.h"
#include "codegen-llvm/llvm_frame.h"
#include "codegen-llvm/llvm_repr.h"
#include "codegen-llvm/llvm_static_slot.h"
#include "il/il.h"

using namespace bronze;
using codegen_llvm::ValueRepr;

namespace {

// One instruction, spelled out. The aggregate initializer the older cases use
// runs to nine positional fields, and every one of these tests cares about two
// of them.
il::Instruction inst(il::Op op, il::Type type, il::ValueId result,
                     std::vector<il::ValueId> operands) {
    il::Instruction i;
    i.op = op;
    i.type = type;
    i.result = result;
    i.operands = std::move(operands);
    return i;
}

il::Instruction boxOf(il::ValueId result, il::ValueId operand, il::Type boxType) {
    il::Instruction i = inst(il::Op::Box, il::Type::Dynamic, result, {operand});
    i.boxType = boxType;
    return i;
}

il::Function oneBlock(std::vector<il::Instruction> body, uint32_t valueCount,
                      std::vector<il::Param> params = {}) {
    il::Function fn;
    fn.name = "f";
    fn.params = std::move(params);
    fn.returnType = il::Type::Void;
    fn.valueCount = valueCount;
    fn.blocks = {{0, {}, std::move(body)}};
    return fn;
}

}  // namespace

// ---- the plan (llvm_repr.h) ------------------------------------------------

TEST_CASE("a box answers for what it boxed, and nothing else answers at all") {
    if (codegen_llvm::reprCodegenDisabled()) return;  // the A/B seam column
    il::Function fn = oneBlock(
        {
            inst(il::Op::ConstF64, il::Type::F64, 0, {}),
            boxOf(1, 0, il::Type::F64),
            boxOf(2, 0, il::Type::I32),
            boxOf(3, 0, il::Type::Bool),
            boxOf(4, 0, il::Type::Str),
            inst(il::Op::ConstUndefined, il::Type::Dynamic, 5, {}),
            inst(il::Op::ConstNull, il::Type::Dynamic, 6, {}),
            inst(il::Op::PropGet, il::Type::Dynamic, 7, {1}),
            inst(il::Op::Ret, il::Type::Void, il::kNoValue, {}),
        },
        8);

    const codegen_llvm::ReprPlan plan = codegen_llvm::planRepr(fn);
    // The f64 box IS the double, canonical NaN and all, which is the whole
    // claim the raw store rests on.
    CHECK(plan.at(1) == ValueRepr::Number);
    // A tag and a payload: not a pointer, and NOT the double slot's bits.
    CHECK(plan.at(2) == ValueRepr::Int32Boxed);
    CHECK(plan.at(3) == ValueRepr::NotPointer);
    // A string box is a heap pointer, so it keeps its root and refuses the
    // raw arm — the one Box that must not be mistaken for the others.
    CHECK(plan.at(4) == ValueRepr::Unknown);
    CHECK(plan.at(5) == ValueRepr::NotPointer);
    CHECK(plan.at(6) == ValueRepr::NotPointer);
    // A property read is the value stage R1 knew nothing about and stage R2
    // still knows nothing about: it can be anything on the heap.
    CHECK(plan.at(7) == ValueRepr::Unknown);
    // %0 is an f64 register, not a `dynamic` value: the plan says nothing
    // about it, because its LLVM type already does.
    CHECK(plan.at(0) == ValueRepr::Unknown);

    CHECK(plan.unrootedValues == 5);  // 1, 2, 3, 5, 6
}

TEST_CASE("a value read only through raw unboxes is a Number, and one read twice is not") {
    if (codegen_llvm::reprCodegenDisabled()) return;
    il::Instruction rawGet = inst(il::Op::PropGet, il::Type::Dynamic, 1, {0});
    il::Instruction rawUnbox = inst(il::Op::Unbox, il::Type::F64, 2, {1});
    rawUnbox.rawUnbox = true;

    il::Instruction mixedGet = inst(il::Op::PropGet, il::Type::Dynamic, 3, {0});
    il::Instruction mixedUnbox = inst(il::Op::Unbox, il::Type::F64, 4, {3});
    mixedUnbox.rawUnbox = true;
    // The second use is an ordinary one, so the value has to survive a
    // collection and keeps its root.
    il::Instruction alsoBoxed = inst(il::Op::StrictEq, il::Type::Bool, 5, {3, 0});

    il::Instruction nullishGet = inst(il::Op::PropGet, il::Type::Dynamic, 6, {0});
    il::Instruction nullishUnbox = inst(il::Op::Unbox, il::Type::F64, 7, {6});
    nullishUnbox.nullishUnbox = true;

    il::Function fn = oneBlock({rawGet, rawUnbox, mixedGet, mixedUnbox, alsoBoxed, nullishGet,
                                nullishUnbox, inst(il::Op::Ret, il::Type::Void, il::kNoValue, {})},
                               8, {{"o", il::Type::Dynamic}});

    const codegen_llvm::ReprPlan plan = codegen_llvm::planRepr(fn);
    CHECK(plan.at(1) == ValueRepr::Number);
    CHECK(plan.at(3) == ValueRepr::Unknown);
    // Number, null or undefined: not a Number, but not a pointer either, so
    // the root goes and the store test stays.
    CHECK(plan.at(6) == ValueRepr::NotPointer);
    CHECK(plan.at(0) == ValueRepr::Unknown);  // a `dynamic` parameter, unproven
}

TEST_CASE("a loop-carried accumulator of boxed doubles keeps its answer around the back edge") {
    if (codegen_llvm::reprCodegenDisabled()) return;
    // %1 = box.f64 %0            ; the preheader's value
    // jump ^loop(%1)
    // ^loop(%2: dynamic):
    //   %3 = box.f64 %0          ; the back edge's value
    //   branch ^loop(%3), ^exit
    //
    // The two edges agree, and the only way to SAY they agree about a value
    // defined in terms of itself is to start at the top and lower — which is
    // what makes the plan a greatest fixpoint rather than a forward walk.
    il::Instruction jump = inst(il::Op::Jump, il::Type::Void, il::kNoValue, {});
    jump.target = il::BlockTarget{1, {1}};
    il::Instruction back = inst(il::Op::Branch, il::Type::Void, il::kNoValue, {2});
    back.target = il::BlockTarget{1, {3}};
    back.elseTarget = il::BlockTarget{2, {}};

    il::Function fn;
    fn.name = "loop";
    fn.returnType = il::Type::Void;
    fn.valueCount = 4;
    fn.blocks = {
        {0, {}, {inst(il::Op::ConstF64, il::Type::F64, 0, {}), boxOf(1, 0, il::Type::F64), jump}},
        {1, {{2, il::Type::Dynamic}}, {boxOf(3, 0, il::Type::F64), back}},
        {2, {}, {inst(il::Op::Ret, il::Type::Void, il::kNoValue, {})}},
    };

    CHECK(codegen_llvm::planRepr(fn).at(2) == ValueRepr::Number);

    // One disagreeing edge and the answer falls, which is the half that keeps
    // the optimism honest.
    fn.blocks[1].instructions[0] = inst(il::Op::PropGet, il::Type::Dynamic, 3, {1});
    CHECK(codegen_llvm::planRepr(fn).at(2) == ValueRepr::Unknown);
}

TEST_CASE("the frame plan drops a root for every value the representation plan settles") {
    if (codegen_llvm::reprCodegenDisabled()) return;
    il::Function fn = oneBlock(
        {
            inst(il::Op::ConstF64, il::Type::F64, 0, {}),
            boxOf(1, 0, il::Type::F64),
            inst(il::Op::PropGet, il::Type::Dynamic, 2, {1}),
            inst(il::Op::PropSet, il::Type::Void, il::kNoValue, {2, 1}),
            inst(il::Op::Ret, il::Type::Void, il::kNoValue, {}),
        },
        3);

    const codegen_llvm::FramePlan proven =
        codegen_llvm::planFrame(fn, /*moduleHasNewTarget=*/false, codegen_llvm::planRepr(fn));
    // The empty plan is what `BRONZE_NO_REPR_CODEGEN=1` produces, so this is
    // the stage R1 column measured against the stage R2 one on one IL.
    const codegen_llvm::FramePlan r1 =
        codegen_llvm::planFrame(fn, /*moduleHasNewTarget=*/false, codegen_llvm::ReprPlan{});
    CHECK(proven.ownSlots < r1.ownSlots);
}

// ---- the store sites (llvm_static_slot.h) -----------------------------------

namespace {

// A module with the one table the identity-form guard reads, and a function to
// emit into. Nothing here runs; what is inspected is the emitted shape.
struct StoreSiteFixture {
    llvm::LLVMContext ctx;
    llvm::Module m{"repr_store_test", ctx};
    codegen_llvm::AbiFns abi{};
    codegen_llvm::ModuleTables tables{};
    llvm::Function* fn = nullptr;

    StoreSiteFixture() {
        codegen_llvm::declareAbiSymbols(m, ctx, abi, /*sharedRuntime=*/false);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* cellsTy = llvm::ArrayType::get(i64Ty, 1);
        tables.staticSlots = new llvm::GlobalVariable(
            m, cellsTy, /*isConstant=*/false, llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantAggregateZero::get(cellsTy), "static_slots");
        tables.staticSlotCount = 1;
        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {i64Ty, i64Ty}, false);
        fn = llvm::Function::Create(fnTy, llvm::GlobalValue::ExternalLinkage, "f", m);
    }

    size_t opcodeCount(unsigned opcode) const {
        size_t n = 0;
        for (const llvm::BasicBlock& bb : *fn) {
            for (const llvm::Instruction& i : bb) {
                if (i.getOpcode() == opcode) ++n;
            }
        }
        return n;
    }

    // Emits one identity-form store site whose value is `repr`, and returns the
    // fixture's own instruction counts afterwards.
    void emitStore(ValueRepr repr, uint32_t slot) {
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(ctx, "done", fn);
        llvm::IRBuilder<> b(entry);
        codegen_llvm::StaticSite site;
        site.slot = slot;
        site.cellIndex = 0;
        codegen_llvm::emitStaticSlotGuard(b, tables, fn->getArg(0), site, done, fn->getArg(1), repr,
                                          "set");
        b.CreateBr(done);
        b.SetInsertPoint(done);
        b.CreateRetVoid();
    }
};

}  // namespace

TEST_CASE("a proven Number is stored raw: no representation load, no test, no branch") {
    StoreSiteFixture f;
    f.emitStore(ValueRepr::Number, /*slot=*/2);
    REQUIRE_FALSE(llvm::verifyFunction(*f.fn, &llvm::errs()));

    // Three loads are the GUARD's — the flags word, the shape word and the
    // published cell. A fourth would be `double_slots`, and its absence is the
    // whole point: the eight bytes a Number box holds are correct for a double
    // slot and for a boxed one alike, so there is nothing left to ask.
    CHECK(f.opcodeCount(llvm::Instruction::Load) == 3);
    CHECK(f.opcodeCount(llvm::Instruction::Store) == 1);
    CHECK(f.opcodeCount(llvm::Instruction::Select) == 0);
    CHECK(f.opcodeCount(llvm::Instruction::SIToFP) == 0);
}

TEST_CASE("an Int32 box converts inline instead of missing to the helper") {
    StoreSiteFixture f;
    f.emitStore(ValueRepr::Int32Boxed, /*slot=*/2);
    REQUIRE_FALSE(llvm::verifyFunction(*f.fn, &llvm::errs()));

    // The representation load is back, because an Int32's bits are a tag and a
    // payload rather than an f64 and the slot's answer decides which word to
    // write.
    CHECK(f.opcodeCount(llvm::Instruction::Load) == 4);
    // `slotReprCanonicalize`'s Int32 case, moved to the site: truncate the
    // payload, convert, take the bits.
    CHECK(f.opcodeCount(llvm::Instruction::SIToFP) == 1);
    CHECK(f.opcodeCount(llvm::Instruction::Trunc) == 1);
    // One store on one path: the choice is a select, not a second arm, so an
    // `i | 0` store never leaves the fast path.
    CHECK(f.opcodeCount(llvm::Instruction::Select) == 1);
    CHECK(f.opcodeCount(llvm::Instruction::Store) == 1);
}

TEST_CASE("an unproven value keeps stage R1's test-and-store arm") {
    StoreSiteFixture f;
    f.emitStore(ValueRepr::Unknown, /*slot=*/2);
    REQUIRE_FALSE(llvm::verifyFunction(*f.fn, &llvm::errs()));

    CHECK(f.opcodeCount(llvm::Instruction::Load) == 4);
    // No conversion: a value that is not known to be a Number may not be
    // written into a double slot at all, so the arm is a refusal and the miss
    // path generalizes the slot instead.
    CHECK(f.opcodeCount(llvm::Instruction::SIToFP) == 0);
    CHECK(f.opcodeCount(llvm::Instruction::Select) == 0);
    CHECK(f.opcodeCount(llvm::Instruction::Store) == 1);
}

TEST_CASE("a slot past the representation word's width never asks about it") {
    // `Shape::double_slots` is 64 bits wide, so slot 64 and up can never be a
    // double one and every arm collapses to the plain store — including the
    // unproven one, which is the case a mask on a compile-time constant folds.
    //
    // Compared against the proven column rather than against a count: a slot
    // that far out lives in the out-of-line block, which costs a load of its
    // own, and pinning an absolute number here would pin THAT instead.
    StoreSiteFixture unproven;
    unproven.emitStore(ValueRepr::Unknown, BRONZE_ABI_SHAPE_DOUBLE_SLOT_LIMIT);
    REQUIRE_FALSE(llvm::verifyFunction(*unproven.fn, &llvm::errs()));

    StoreSiteFixture proven;
    proven.emitStore(ValueRepr::Number, BRONZE_ABI_SHAPE_DOUBLE_SLOT_LIMIT);
    REQUIRE_FALSE(llvm::verifyFunction(*proven.fn, &llvm::errs()));

    CHECK(unproven.opcodeCount(llvm::Instruction::Load) ==
          proven.opcodeCount(llvm::Instruction::Load));
    CHECK(unproven.opcodeCount(llvm::Instruction::Store) == 1);
    CHECK(unproven.opcodeCount(llvm::Instruction::Select) == 0);
}
