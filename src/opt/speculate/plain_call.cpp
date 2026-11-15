// PLAIN CALL: `r = call.dynamic %callee, %this, a0, ..., a(n-1)` whose callee
// was always one module function f.
//
// Evidence (evidence.cpp planCall): a recorded Target site that saw only
// functions, and only one, named `<name>@<fnv32 of its source>` and matched
// to exactly one IL function; and f's convention fits a direct call with n
// arguments (policy.h directCallFits). A harvest has no call cache, so it
// never plans one; the spread form (`call.dynamic.spread`) is never planned.
//
//     guard fn.code_is %callee, @entry(f)
//     r = call @entry(f)(...)               ; direct_call.cpp
//
// Why exact: `fn.code_is` is total and pure (false for anything that is not
// a function object of f's code), so a failure leaves before anything ran and
// G re-runs the `call.dynamic` whole. On the passing edge %callee is a closure
// of f, and calling it through the uniform path is the direct call above
// (direct_call.cpp says why each operand is what the wrapper would bind).

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewritePlainCall(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::Call && plan.callee != kNoFunction);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction& site = b.site();
    assert(site.op == il::Op::DynamicCall && site.operands.size() >= 2);
    const ValueId callee = site.operands[0];
    const ValueId thisValue = site.operands[1];
    const std::vector<ValueId> args(site.operands.begin() + 2, site.operands.end());

    b.guard(emitCodeIs(b, callee, plan.callee));
    emitDirectCall(b, plan.callee, callee, thisValue, args);
    b.finish();
    ++ctx.stats.plainCalls;
}

}  // namespace bronze::opt::speculate
