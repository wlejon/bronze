// METHOD CALL: `r = method.call %recv, <key>, a0, ..., a(n-1)` whose method
// was always one module function f. The generic op is ECMA-262's
// `recv[key](...)`: GetValue of the property, then Call with %recv as `this`
// (runtime/rt_method_call.cpp bronze_call_method: bronze_prop_get, then
// bronze_dynamic_call — its caches answer the same two steps faster).
//
// Evidence (evidence.cpp planMethod): one callee over every way of the cache
// (`direct` or `slot`, never `*other*`) or a recorded Method site's one
// target, matched to exactly one IL function whose convention fits a direct
// call with n arguments (policy.h). The spread form is never planned.
//
// MethodSlot — the method is an own data property of the receiver's one
// shape (a harvest's lone `slot` way, or a recorded shape listing the key):
//
//     guard is.object %recv
//     guard ptr.eq (load.shape %recv), (shape.cell n)
//     m = slot.load %recv, k
//     guard fn.code_is m, @entry(f)               ; leaves IN FRONT of the site
//     r = call @entry(f)(...)
//
// Everything before the last guard is a test or a load an own data property
// licenses (prop_read.cpp says why the slot is the property), so a failure at
// any of them leaves with nothing observable done and G re-runs the whole
// `method.call`.
//
// MethodProto — anything else with one callee, typically a method found on
// the prototype chain:
//
//     m = prop.get %recv, <key>, <a fresh IC>     ; the generic read
//     br (fn.code_is m, @entry(f)), fast, exit
//   fast:  r = call @entry(f)(...)
//   exit:  t = call.dynamic m, %recv, a0, ...      ; finishes the generic op
//          leave @G, <point AFTER the site>, state with r := t
//
// The read cannot be followed by an exit IN FRONT of the site: on the
// passing evidence it hit a data property, but the prototype chain is the
// program's to change, and a getter installed since would run here and again
// in G — an observable double read. So the failing edge does not undo the
// read; it finishes the operation exactly as bronze_call_method would (the
// same value as callee, the same receiver, the same arguments) and resumes G
// AFTER the `method.call`, with the call's result in the result's place. No
// receiver-shape guard is needed: the read is generic and the identity test
// is what licenses the direct call. The read gets an inline-cache site of its
// own (context.h newIcSite): a method site's words mean something else and
// may not be shared with a property read (il.h `methodIcSites`).

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewriteMethodCall(RewriteContext& ctx, const SitePlan& plan) {
    assert((plan.kind == SiteKind::MethodSlot || plan.kind == SiteKind::MethodProto) && plan.callee != kNoFunction);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction& site = b.site();
    assert(site.op == il::Op::MethodCall && !site.operands.empty() && site.type == il::Type::Dynamic);
    const ValueId receiver = site.operands[0];
    const std::vector<ValueId> args(site.operands.begin() + 1, site.operands.end());

    if (plan.kind == SiteKind::MethodSlot) {
        assert(plan.shapes.size() == 1);
        guardShape(b, receiver, plan.shapes.front().key);
        const ValueId method = b.emit(il::Op::SlotLoad, il::Type::Dynamic, {receiver}, plan.shapes.front().slot);
        b.guard(emitCodeIs(b, method, plan.callee));
        emitDirectCall(b, plan.callee, method, receiver, args);
        b.finish();
        ++ctx.stats.methodCalls;
        return;
    }

    il::Instruction read;
    read.op = il::Op::PropGet;
    read.type = il::Type::Dynamic;
    read.operands = {receiver};
    read.keyIndex = site.keyIndex;
    read.icIndex = ctx.newIcSite();
    const ValueId method = b.emitInst(std::move(read));

    // The exit's work: the rest of the generic operation.
    il::Instruction rest;
    rest.op = il::Op::DynamicCall;
    rest.type = il::Type::Dynamic;
    rest.result = b.freshValue(il::Type::Dynamic);
    rest.operands = {method, receiver};
    rest.operands.insert(rest.operands.end(), args.begin(), args.end());
    const ValueId restResult = rest.result;
    std::vector<il::Instruction> work;
    work.push_back(std::move(rest));
    const BlockId exit = b.exitAfter(std::move(work), restResult);

    b.guardTo(emitCodeIs(b, method, plan.callee), true, exit);
    emitDirectCall(b, plan.callee, method, receiver, args);
    b.finish();
    ++ctx.stats.methodCalls;
}

}  // namespace bronze::opt::speculate
