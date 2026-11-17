// CONSTRUCT: `r = construct %F, a0, ..., a(n-1)` whose constructor was always
// one module function f, which G's own fast path entered (evidence.cpp
// planConstruct: the harvest's callee turns of the site).
//
//     guard fn.code_is %F, @entry(f)
//     guard fn.constructs %F
//     s = fn.instance_shape %F
//     this = create.this %F, s
//     v = call @entry(f)(env?, this, a0, ...)     ; direct_call.cpp
//     r = construct.result v, this
//
// Why exact: the two guards are total and pure, so a failure leaves before
// anything ran and G re-runs the `construct` whole. Past them %F is a
// closure of f of this realm that the runtime has constructed with its plain
// path (the construct-vetted byte: set only there, never for a bound,
// wrapper or native-base constructor, and answered by the code pointer, so
// it never goes stale), so OrdinaryCreateFromConstructor makes an ordinary
// object of %F's instance shape: `create.this`. The shape is read at every
// construction: an assignment to `F.prototype` replaces it (rt_prop_write.cpp)
// and the next instance gets the new one. f reads no
// new.target and calls no super() (policy.h constructFits), so nothing in its
// body can tell the direct call from the runtime's [[Construct]]; the
// result is 10.2.2's: what f returned if it is an object, else the instance.
//
// The call carries the point after the generic `construct` as its
// `resumeAfter`, like any direct call the rewrites make: the inliner finishes
// a guard of f's inlined body in f's G and applies the same result rule
// before G continues (opt/inline splice.cpp).
//
// Derived constructors (`class D extends B` with a constructor) are not
// expressible: their instance is made by the base's [[Construct]] that
// `super()` performs with new.target = D, a scope only the runtime's
// construct pushes (functionEntersDirectly refuses super()). Speculating them
// needs a direct form of super() that takes new.target as an operand.

#include "opt/speculate/policy.h"
#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewriteConstruct(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::Construct && plan.callee != kNoFunction);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction& site = b.site();
    assert(site.op == il::Op::Construct && !site.operands.empty());
    const ValueId ctor = site.operands[0];
    const std::vector<ValueId> args(site.operands.begin() + 1, site.operands.end());
    assert(constructFits(ctx.module, ctx.module.functions[plan.callee], args.size()));

    b.guard(emitCodeIs(b, ctor, plan.callee));
    b.guard(b.emit(il::Op::FnConstructs, il::Type::Bool, {ctor}));
    const ValueId shape = b.emit(il::Op::FnInstanceShape, il::Type::Ptr, {ctor});
    // The instance is the construct's allocation: it keeps the construct's
    // slow site, which says whether the site is pretenured
    // (il.h Module::pretenureSites).
    il::Instruction create;
    create.op = il::Op::CreateThis;
    create.type = il::Type::Dynamic;
    create.operands = {ctor, shape};
    create.slowSite = site.slowSite;
    create.span = site.span;
    const ValueId self = b.emitInst(std::move(create));
    const ValueId returned = emitDirectCallValue(b, plan.callee, ctor, self, args);
    if (site.result != il::kNoValue) {
        il::Instruction result;
        result.op = il::Op::ConstructResult;
        result.type = il::Type::Dynamic;
        result.operands = {returned, self};
        b.emitResult(std::move(result));
    }
    b.finish();
    ++ctx.stats.constructs;
}

}  // namespace bronze::opt::speculate
