// THE GUARDED-REGION FRAME OVERLAY (codegen-llvm/llvm_frame.h).
//
// The fast and slow copies of one region are mutually exclusive, so their
// pinned `dynamic` values share GC root slots: both copies are laid out from
// the same base and the frame advances by the LARGER of the two counts.
//
// Checked against hand-built IL rather than a compiled fixture, for the reason
// `repr_test.cpp` gives about the plan beside it: a slot index read off nine
// instructions is exact, where the same index read off a compiled program is
// arithmetic on a count nobody can see.

#include <doctest/doctest.h>

#include <vector>

#include "codegen-llvm/llvm_frame.h"
#include "codegen-llvm/llvm_repr.h"
#include "il/il.h"
#include "support/diagnostics.h"

using namespace bronze;

namespace {

il::Instruction inst(il::Op op, il::Type type, il::ValueId result,
                     std::vector<il::ValueId> operands) {
    il::Instruction i;
    i.op = op;
    i.type = type;
    i.result = result;
    i.operands = std::move(operands);
    return i;
}

il::Instruction alloc(il::ValueId result) {
    return inst(il::Op::CreateObject, il::Type::Dynamic, result, {});
}

il::Instruction jump(il::BlockId target) {
    il::Instruction i;
    i.op = il::Op::Jump;
    i.target.block = target;
    return i;
}

il::Instruction branch(il::ValueId cond, il::BlockId thenBlock, il::BlockId elseBlock) {
    il::Instruction i;
    i.op = il::Op::Branch;
    i.operands = {cond};
    i.target.block = thenBlock;
    i.elseTarget.block = elseBlock;
    return i;
}

il::Block block(il::BlockId id, std::vector<il::Instruction> body) {
    il::Block b;
    b.id = id;
    b.instructions = std::move(body);
    return b;
}

// One shared pinned value, three pinned in a region's fast copy and two in its
// slow copy. Every pinned value is an allocation whose result is read in the
// NEXT block, which is exactly the rule that gives a value a slot of its own;
// everything else here is a block-local temporary out of the shared pool.
//
//   b0 [shared]  %0 = create.object            read in b2 and b4
//   b1 [fast 0]  %2 %3 %4 = create.object      read in b2
//   b2 [fast 0]  the fast copy's arithmetic
//   b3 [slow 0]  %7 %8 = create.object         read in b4
//   b4 [slow 0]  the slow copy's arithmetic
il::Function twoCopies(bool withCopyClasses) {
    il::Function fn;
    fn.name = "f";
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 12;

    il::Block b0 = block(0, {alloc(0),
                             inst(il::Op::ConstBool, il::Type::Bool, 1, {}),
                             branch(1, 1, 3)});
    il::Block b1 = block(1, {alloc(2), alloc(3), alloc(4), jump(2)});
    il::Block b2 = block(2, {inst(il::Op::Add, il::Type::Dynamic, 5, {2, 3}),
                             inst(il::Op::Add, il::Type::Dynamic, 6, {5, 4}),
                             inst(il::Op::Add, il::Type::Dynamic, 10, {6, 0}),
                             inst(il::Op::Ret, il::Type::Void, il::kNoValue, {10})});
    il::Block b3 = block(3, {alloc(7), alloc(8), jump(4)});
    il::Block b4 = block(4, {inst(il::Op::Add, il::Type::Dynamic, 9, {7, 8}),
                             inst(il::Op::Add, il::Type::Dynamic, 11, {9, 0}),
                             inst(il::Op::Ret, il::Type::Void, il::kNoValue, {11})});

    if (withCopyClasses) {
        b1.copyClass = il::CopyClass::Fast;
        b1.copyRegion = 0;
        b2.copyClass = il::CopyClass::Fast;
        b2.copyRegion = 0;
        b3.copyClass = il::CopyClass::Slow;
        b3.copyRegion = 0;
        b4.copyClass = il::CopyClass::Slow;
        b4.copyRegion = 0;
    }

    fn.blocks = {std::move(b0), std::move(b1), std::move(b2), std::move(b3), std::move(b4)};
    return fn;
}

codegen_llvm::FramePlan planOf(const il::Function& fn) {
    return codegen_llvm::planFrame(fn, /*moduleHasNewTarget=*/false, codegen_llvm::planRepr(fn));
}

}  // namespace

TEST_CASE("the two copies of one region are laid out from the same base") {
    const codegen_llvm::FramePlan overlaid = planOf(twoCopies(/*withCopyClasses=*/true));
    const codegen_llvm::FramePlan flat = planOf(twoCopies(/*withCopyClasses=*/false));

    // The shared value comes first, as it always did.
    CHECK(overlaid.slotOf[0] == 0);
    // Then the region's band: the fast copy's three from the base, and the slow
    // copy's two from THE SAME base. Slot 1 holds %2 on a run that took the
    // fast copy and %7 on one that did not, and no run can want both.
    CHECK(overlaid.slotOf[2] == 1);
    CHECK(overlaid.slotOf[3] == 2);
    CHECK(overlaid.slotOf[4] == 3);
    CHECK(overlaid.slotOf[7] == 1);
    CHECK(overlaid.slotOf[8] == 2);

    // The pinned band is 1 + max(3, 2) and not 1 + 3 + 2, so the pool that sits
    // immediately above it starts two slots lower. The pool itself is unchanged
    // — two block-local temporaries at the widest block either way.
    CHECK(overlaid.argvBase == 6);
    CHECK(flat.argvBase == 8);
    CHECK(overlaid.ownSlots == 6);
    CHECK(flat.ownSlots == 8);

    // Without the classes every one of those values is shared, which is the
    // layout this chunk started from and the one `BRONZE_NO_GUARDED_REGIONS=1`
    // still produces.
    CHECK(flat.slotOf[0] == 0);
    CHECK(flat.slotOf[2] == 1);
    CHECK(flat.slotOf[7] == 4);

    CHECK_FALSE(overlaid.crossCopyUse);
    CHECK_FALSE(flat.crossCopyUse);
}

TEST_CASE("two regions of one function get disjoint bases") {
    // The same body with b3/b4 belonging to a SECOND region rather than to the
    // slow copy of the first. Two regions are two loops, either of which may
    // run, so only the two copies of ONE of them may share slots.
    il::Function fn = twoCopies(/*withCopyClasses=*/true);
    fn.blocks[3].copyRegion = 1;
    fn.blocks[4].copyRegion = 1;

    const codegen_llvm::FramePlan plan = planOf(fn);
    CHECK(plan.slotOf[0] == 0);
    CHECK(plan.slotOf[2] == 1);
    CHECK(plan.slotOf[4] == 3);
    // Region 1's band starts where region 0's ended.
    CHECK(plan.slotOf[7] == 4);
    CHECK(plan.slotOf[8] == 5);
    CHECK(plan.ownSlots == 8);
}

TEST_CASE("a value read from the other copy trips the frame planner") {
    // %2 is defined in the fast copy and read in the slow one. Nothing the pass
    // emits can look like this — the verifier refuses such a module before the
    // backend ever runs — so what this pins is the tripwire, which is what
    // stands between a bug in the pass and a frame slot two live values share.
    il::Function fn = twoCopies(/*withCopyClasses=*/true);
    fn.blocks[4].instructions[1].operands[1] = 2;

    DiagnosticSink diags;
    const codegen_llvm::FramePlan plan =
        codegen_llvm::planFrame(fn, /*moduleHasNewTarget=*/false, codegen_llvm::planRepr(fn),
                                codegen_llvm::LiveRootPlan{}, &diags);
    CHECK(plan.crossCopyUse);
    REQUIRE(diags.hasErrors());
    const std::string message = diags.all().front().message;
    CHECK(message.find("%2") != std::string::npos);
    CHECK(message.find("fast 0") != std::string::npos);
    CHECK(message.find("slow 0") != std::string::npos);
}
