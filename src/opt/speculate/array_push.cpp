// ARRAY PUSH: `r = method.call %a, "push", %v` where %a is an array the
// function made (`array.literal`, `create.array`), the commonest way a loop
// collects its results.
//
// Evidence (evidence.cpp planMethod): the allocation. An array a literal
// makes stays at the bare array shape unless the program gives it a named
// property or another prototype, which is what the guard tests:
//
//     guard is.plain_array %a
//     r = array.push %a, %v
//
// On the bare shape `push` can only be %Array.prototype.push% and the Set it
// makes can reach no accessor (il_op.h is.plain_array), so the generic
// operation is exactly the append `array.push` does, and a failing guard has
// done nothing: it leaves in front of the site and G makes the call. What the
// form buys is its effect: the generic call runs unknown code and so ends
// every fact about memory, where `array.push` writes the one array's length
// and elements, so a loop that pushes keeps the facts (and the hoisted tests)
// about every other array it reads.

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewriteArrayPush(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::ArrayPush);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction site = b.site();
    assert(site.op == il::Op::MethodCall && site.operands.size() >= 2 && site.operands.size() <= 9);
    const ValueId array = site.operands[0];
    b.guard(b.emit(il::Op::IsPlainArray, il::Type::Bool, {array}));
    il::Instruction push;
    push.op = il::Op::ArrayPush;
    push.type = il::Type::Dynamic;
    push.operands = site.operands;
    if (site.result != il::kNoValue) {
        b.emitResult(std::move(push));
    } else {
        b.emitInst(std::move(push));
    }
    b.finish();
    ++ctx.stats.arrayPushes;
}

}  // namespace bronze::opt::speculate
