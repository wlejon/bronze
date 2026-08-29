// THE LIVE-ROOT PLAN (codegen-llvm/llvm_live_roots.h).
//
// Two answers about one function, both read off hand-built IL rather than off a
// compiled program, for the reason `repr_test.cpp` and `frame_overlay_test.cpp`
// give: a slot index and a reload decision read off eight instructions are
// exact, where the same facts read off a compiled fixture are arithmetic on
// counts nobody can see.
//
// The shapes below are the ones the plan has to get right, and each is written
// so that getting it wrong is a use-after-move rather than a slow program.

#include <doctest/doctest.h>

#include <vector>

#include "codegen-llvm/llvm_frame.h"
#include "codegen-llvm/llvm_live_roots.h"
#include "codegen-llvm/llvm_repr.h"
#include "il/il.h"

using namespace bronze;
using codegen_llvm::LiveRootPlan;

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

// An allocation: `il::canCollect` admits it, so it is the safepoint every case
// below is built around.
il::Instruction alloc(il::ValueId result) {
    return inst(il::Op::CreateObject, il::Type::Dynamic, result, {});
}

// A use that neither allocates nor throws: one unsigned compare on bits already
// in a register (src/il/effects.cpp).
il::Instruction isNumber(il::ValueId result, il::ValueId of) {
    return inst(il::Op::IsNumber, il::Type::Bool, result, {of});
}

il::Instruction jump(il::BlockId target, std::vector<il::ValueId> args = {}) {
    il::Instruction i;
    i.op = il::Op::Jump;
    i.target.block = target;
    i.target.args = std::move(args);
    return i;
}

il::Instruction branch(il::ValueId cond, il::BlockId thenBlock, il::BlockId elseBlock,
                       std::vector<il::ValueId> thenArgs = {}) {
    il::Instruction i;
    i.op = il::Op::Branch;
    i.operands = {cond};
    i.target.block = thenBlock;
    i.target.args = std::move(thenArgs);
    i.elseTarget.block = elseBlock;
    return i;
}

il::Instruction ret(il::ValueId v) {
    return inst(il::Op::Ret, il::Type::Void, il::kNoValue, {v});
}

il::Block block(il::BlockId id, std::vector<il::Instruction> body) {
    il::Block b;
    b.id = id;
    b.instructions = std::move(body);
    return b;
}

}  // namespace

TEST_CASE("a use with nothing collecting between it and the def reads the register") {
    //   b0: %0 = create.object      the safepoint that gives %0 its slot
    //       %1 = create.object
    //       %2 = is.number %1       nothing has collected since %1's def
    //       %3 = is.number %1       nor since the use above it
    //       ret %0
    il::Function fn;
    fn.name = "f";
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 4;
    fn.blocks = {block(0, {alloc(0), alloc(1), isNumber(2, 1), isNumber(3, 1), ret(0)})};

    const LiveRootPlan plan = codegen_llvm::planLiveRoots(fn);
    // %1 is live across nothing that collects, so it needs no slot at all — and
    // a use of a slotless value is not a reload question in the first place.
    CHECK(plan.rooted(1) == false);
    CHECK(plan.anchor(0, 2, 0) == LiveRootPlan::kReload);
    // %0 IS live across %1's allocation, so it keeps its slot; its use at the
    // `ret` is a reload, because that allocation stands between.
    CHECK(plan.rooted(0));
    CHECK(plan.anchor(0, 4, 0) == LiveRootPlan::kReload);

    // And the frame agrees: one slot, not two.
    const codegen_llvm::FramePlan frame =
        codegen_llvm::planFrame(fn, /*moduleHasNewTarget=*/false, codegen_llvm::planRepr(fn), plan);
    CHECK(frame.slotOf[0] != codegen_llvm::kNoFrameSlot);
    CHECK(frame.slotOf[1] == codegen_llvm::kNoFrameSlot);
}

TEST_CASE("a use after an allocation in the same block reloads, and the one after it does not") {
    //   b0: %0 = create.object
    //       %1 = is.number %0     no collection since the def: register
    //       %2 = create.object    a safepoint
    //       %3 = is.number %0     the register is stale: reload
    //       %4 = is.number %0     the reload above made it current again
    //       ret %2
    il::Function fn;
    fn.name = "f";
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 5;
    fn.blocks = {block(0, {alloc(0), isNumber(1, 0), alloc(2), isNumber(3, 0), isNumber(4, 0),
                           ret(2)})};

    const LiveRootPlan plan = codegen_llvm::planLiveRoots(fn);
    REQUIRE(plan.rooted(0));
    CHECK(plan.anchor(0, 1, 0) == 0);  // the def's own block, no load
    CHECK(plan.anchor(0, 3, 0) == LiveRootPlan::kReload);
    CHECK(plan.anchor(0, 4, 0) == 0);
}

TEST_CASE("a use reached by one path that collects and one that does not reloads") {
    //   b0: %0 = create.object
    //       %1 = const.bool
    //       br %1, b1, b2
    //   b1: %2 = create.object      the path that collects
    //       jump b3
    //   b2: jump b3                 the path that does not
    //   b3: %3 = is.number %0       two predecessors: the slot, always
    //       ret %0
    il::Function fn;
    fn.name = "f";
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 4;
    fn.blocks = {block(0, {alloc(0), inst(il::Op::ConstBool, il::Type::Bool, 1, {}),
                           branch(1, 1, 2)}),
                 block(1, {alloc(2), jump(3)}),
                 block(2, {jump(3)}),
                 block(3, {isNumber(3, 0), ret(0)})};

    const LiveRootPlan plan = codegen_llvm::planLiveRoots(fn);
    REQUIRE(plan.rooted(0));
    CHECK(plan.anchor(3, 0, 0) == LiveRootPlan::kReload);
    // The block with ONE predecessor and nothing collecting in front of it does
    // carry the register across the edge, which is the case the join above is
    // being distinguished from.
    CHECK(plan.anchor(2, 0, 0) == LiveRootPlan::kReload);  // b2's jump takes no arguments
}

TEST_CASE("a straight-line chain carries the register across its blocks") {
    //   b0: %0 = create.object
    //       %1 = create.object     the safepoint that dirties %0
    //       jump b1
    //   b1: %2 = is.number %0      one predecessor, nothing collected: reload
    //       jump b2
    //   b2: %3 = is.number %0      still the chain, and b1 made it current
    //       ret %1
    il::Function fn;
    fn.name = "f";
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 4;
    fn.blocks = {block(0, {alloc(0), alloc(1), jump(1)}),
                 block(1, {isNumber(2, 0), jump(2)}),
                 block(2, {isNumber(3, 0), ret(1)})};

    const LiveRootPlan plan = codegen_llvm::planLiveRoots(fn);
    REQUIRE(plan.rooted(0));
    CHECK(plan.anchor(1, 0, 0) == LiveRootPlan::kReload);
    CHECK(plan.anchor(2, 0, 0) == 1);  // the register b1's reload left
}

TEST_CASE("a value live into an exception handler is rooted in front of the throwing use") {
    //   b0 [handler b1]:
    //       %1 = create.object     the last thing that collects before %0's def
    //       %0 = create.object
    //       %2 = is.number %0      a use that moves nothing
    //       %3 = prop.get %1       can throw, and minting the error allocates
    //       ret %3
    //   b1: %4 = is.number %0      the handler reads %0, and moves nothing
    //       jump b2
    //   b2: %5 = const.undefined
    //       ret %5
    //
    // On the straight path %0 dies at `is.number`, and nothing there collects
    // while it is live. Only the exception edge keeps it alive across the
    // allocation the throw performs, so only the exception edge is the reason
    // it needs a slot — which is what makes this shape the test of that rule
    // rather than of the conservative one beside it.
    il::Function fn;
    fn.name = "f";
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 6;
    il::Block b0 = block(0, {alloc(1), alloc(0), isNumber(2, 0),
                             inst(il::Op::PropGet, il::Type::Dynamic, 3, {1}), ret(3)});
    b0.handler = 1;
    fn.blocks = {std::move(b0), block(1, {isNumber(4, 0), jump(2)}),
                 block(2, {inst(il::Op::ConstUndefined, il::Type::Dynamic, 5, {}), ret(5)})};

    const LiveRootPlan plan = codegen_llvm::planLiveRoots(fn);
    CHECK(plan.rooted(0));
    // The handler is entered from an arbitrary point inside its block, so
    // nothing it reads may come out of a register.
    CHECK(plan.anchor(1, 0, 0) == LiveRootPlan::kReload);
}

TEST_CASE("the pin barrier's violating arm roots what its handler reads") {
    //   b0 [handler b1]:
    //       %2 = create.object      the last thing that collects before %0
    //       %0 = create.object
    //       %1 = const.f64
    //       pin.guard %1, number    answers no to canThrow AND canCollect, and
    //       %3 = is.number %2       still mints a TypeError on the way out
    //       ret %3
    //   b1: %4 = is.number %0
    //       jump b2
    //   b2: %5 = const.undefined
    //       ret %5
    //
    // Nothing on the kept path collects while %0 is live, so without the
    // barrier's own edge %0 would take no slot — and the handler would read a
    // register the TypeError's allocation had already invalidated.
    il::Function fn;
    fn.name = "f";
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 6;
    il::Instruction guard = inst(il::Op::PinGuard, il::Type::Void, il::kNoValue, {1});
    guard.immI32 = static_cast<int32_t>(il::PinBarrier::Number);
    REQUIRE_FALSE(il::canThrow(guard));
    REQUIRE_FALSE(il::canCollect(guard));

    il::Block b0 = block(0, {alloc(2), alloc(0),
                             inst(il::Op::ConstF64, il::Type::Dynamic, 1, {}), guard,
                             isNumber(3, 2), ret(3)});
    b0.handler = 1;
    fn.blocks = {std::move(b0), block(1, {isNumber(4, 0), jump(2)}),
                 block(2, {inst(il::Op::ConstUndefined, il::Type::Dynamic, 5, {}), ret(5)})};

    const LiveRootPlan plan = codegen_llvm::planLiveRoots(fn);
    CHECK(plan.rooted(0));
}

TEST_CASE("a block argument passed after a safepoint reloads at the branch") {
    //   b0: %0 = create.object
    //       %1 = is.number %0       the register, no load
    //       %2 = create.object      the safepoint
    //       br %1, b1(%0), b2       the argument is stale here: reload
    //   b1(%3): ret %3              the trampoline's parameter
    //   b2: ret %2
    il::Function fn;
    fn.name = "f";
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 4;
    il::Block b1;
    b1.id = 1;
    b1.params = {il::BlockParam{3, il::Type::Dynamic}};
    b1.instructions = {ret(3)};
    fn.blocks = {block(0, {alloc(0), isNumber(1, 0), alloc(2), branch(1, 1, 2, {0})}),
                 std::move(b1), block(2, {ret(2)})};

    const LiveRootPlan plan = codegen_llvm::planLiveRoots(fn);
    REQUIRE(plan.rooted(0));
    CHECK(plan.anchor(0, 1, 0) == 0);
    // The branch's uses are enumerated operands-first: use 0 is the condition,
    // use 1 is the `target` argument list.
    CHECK(plan.anchor(0, 3, 1) == LiveRootPlan::kReload);
}

TEST_CASE("a value the representation plan already unrooted stays unrooted") {
    //   b0: %0 = const.f64
    //       %1 = box.f64 %0           a boxed double: never a pointer
    //       %2 = create.object        a safepoint %1 is live across
    //       %3 = is.number %1
    //       ret %2
    il::Function fn;
    fn.name = "f";
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 4;
    il::Instruction boxed = inst(il::Op::Box, il::Type::Dynamic, 1, {0});
    boxed.boxType = il::Type::F64;
    fn.blocks = {block(0, {inst(il::Op::ConstF64, il::Type::F64, 0, {}), std::move(boxed),
                           alloc(2), isNumber(3, 1), ret(2)})};

    const LiveRootPlan plan = codegen_llvm::planLiveRoots(fn);
    // The live plan alone would root it: it IS live across the allocation.
    CHECK(plan.rooted(1));
    // The two plans together do not, and that is the answer the frame takes.
    const codegen_llvm::FramePlan frame =
        codegen_llvm::planFrame(fn, /*moduleHasNewTarget=*/false, codegen_llvm::planRepr(fn), plan);
    CHECK(frame.slotOf[1] == codegen_llvm::kNoFrameSlot);
}

TEST_CASE("an operand of a collecting instruction is rooted even where it dies there") {
    //   b0: %0 = create.object
    //       %1 = prop.get %0     %0's last use, and the helper holds it in a
    //       ret %1               register of its own while it allocates
    il::Function fn;
    fn.name = "f";
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 2;
    fn.blocks = {block(0, {alloc(0), inst(il::Op::PropGet, il::Type::Dynamic, 1, {0}), ret(1)})};

    const LiveRootPlan plan = codegen_llvm::planLiveRoots(fn);
    CHECK(plan.rooted(0));
}
