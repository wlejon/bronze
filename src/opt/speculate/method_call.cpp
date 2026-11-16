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
//     br (fn.code_is m, @entry(f)), direct, other
//   direct: r1 = call @entry(f)(...) ; jump join(r1)
//   other:  r2 = call.dynamic m, %recv, a0, ... ; jump join(r2)   (cold)
//
// Everything before the identity test is a test or a load an own data
// property licenses (prop_read.cpp says why the slot is the property), so a
// failure at either guard leaves with nothing observable done and G re-runs
// the whole `method.call`. The identity test does not leave: the slot holds
// the method, so calling whatever it holds is the generic operation's Call. A
// harvest's `slot` way names the callee the cache latched first, and the
// cache's hits load the slot without comparing it, so objects of one shape
// holding different functions there (one setter per uniform type) are no
// evidence against it; they take the cold arm and stay in S.
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
//
// A site whose harvest named 2..kMaxChainShapes callees (a polymorphic call
// over classes, each with its own method) tests the fetched method against
// each in turn, and each passing test enters its callee directly:
//
//     m = prop.get %recv, <key>, <a fresh IC>
//     br (fn.code_is m, @entry(f1)), arm1, next1
//   arm1:  r1 = call @entry(f1)(...) ; jump join(r1)
//   next1: br (fn.code_is m, @entry(f2)), arm2, exit
//   ...
// The same reasoning holds per arm; only the last test's failure exits.
//
// When a way of the harvest was a `slot` way (SitePlan::calleesOpen: the
// cache never compared that callee after latching it), the last failure is
// not evidence against S: it makes the same generic call in a cold arm into
// the join and stays in S.

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewriteMethodCall(RewriteContext& ctx, const SitePlan& plan) {
    assert((plan.kind == SiteKind::MethodSlot || plan.kind == SiteKind::MethodProto) &&
           (plan.callee != kNoFunction || plan.callees.size() > 1));
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction& site = b.site();
    assert(site.op == il::Op::MethodCall && !site.operands.empty() && site.type == il::Type::Dynamic);
    const ValueId receiver = site.operands[0];
    const std::vector<ValueId> args(site.operands.begin() + 1, site.operands.end());

    if (plan.kind == SiteKind::MethodSlot) {
        assert(plan.shapes.size() == 1);
        guardShape(b, receiver, plan.shapes.front().key);
        const ValueId method = b.emit(il::Op::SlotLoad, il::Type::Dynamic, {receiver}, plan.shapes.front().slot);
        const BlockId direct = b.newBlock();
        const BlockId other = b.newBlock();
        const BlockId join = b.newBlock();
        ctx.speculated.blocks[other].cold = true;
        if (site.result != il::kNoValue) b.resultParam(join);
        b.branch(emitCodeIs(b, method, plan.callee), direct, {}, other, {});
        b.moveTo(direct);
        const ValueId r = emitDirectCallValue(b, plan.callee, method, receiver, args);
        b.jump(join, site.result != il::kNoValue ? std::vector<ValueId>{r} : std::vector<ValueId>{});
        b.moveTo(other);
        il::Instruction call;
        call.op = il::Op::DynamicCall;
        call.type = il::Type::Dynamic;
        call.operands = {method, receiver};
        call.operands.insert(call.operands.end(), args.begin(), args.end());
        const ValueId t = b.emitInst(std::move(call));
        b.jump(join, site.result != il::kNoValue ? std::vector<ValueId>{t} : std::vector<ValueId>{});
        b.moveTo(join);
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

    // The rest of the generic operation, for a method none of the tests
    // named: in the exit, which then leaves after the site, or (calleesOpen)
    // in a cold arm into the join.
    auto rest = [&]() {
        il::Instruction call;
        call.op = il::Op::DynamicCall;
        call.type = il::Type::Dynamic;
        call.result = b.freshValue(il::Type::Dynamic);
        call.operands = {method, receiver};
        call.operands.insert(call.operands.end(), args.begin(), args.end());
        return call;
    };
    const BlockId join = b.newBlock();
    if (site.result != il::kNoValue) b.resultParam(join);
    BlockId exit = il::kNoBlock;
    if (plan.calleesOpen) {
        exit = b.newBlock();
        ctx.speculated.blocks[exit].cold = true;
        const BlockId arms = b.cursor();
        b.moveTo(exit);
        const ValueId t = b.emitInst(rest());
        b.jump(join, site.result != il::kNoValue ? std::vector<ValueId>{t} : std::vector<ValueId>{});
        b.moveTo(arms);
    } else {
        il::Instruction call = rest();
        const ValueId restResult = call.result;
        std::vector<il::Instruction> work;
        work.push_back(std::move(call));
        exit = b.exitAfter(std::move(work), restResult);
    }

    // One identity test per arm, in the harvest's order, the last one's
    // failure going to `exit`; each arm a direct call into the join.
    std::vector<uint32_t> callees = plan.callees;
    if (callees.empty()) callees.push_back(plan.callee);
    for (size_t k = 0; k < callees.size(); ++k) {
        const BlockId arm = b.newBlock();
        const bool last = k + 1 == callees.size();
        const BlockId next = last ? exit : b.newBlock();
        b.branch(emitCodeIs(b, method, callees[k]), arm, {}, next, {});
        if (!plan.calleesOpen) ++ctx.stats.guards;
        b.moveTo(arm);
        const ValueId r = emitDirectCallValue(b, callees[k], method, receiver, args);
        b.jump(join, site.result != il::kNoValue ? std::vector<ValueId>{r} : std::vector<ValueId>{});
        if (!last) b.moveTo(next);
    }
    b.moveTo(join);
    b.finish();
    ++ctx.stats.methodCalls;
}

}  // namespace bronze::opt::speculate
