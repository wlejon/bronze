// MATH INTRINSICS: `r = method.call %Math, <key>, a0[, a1]` whose callee was
// always one Math function the runtime exports (il::Intrinsic).
//
// Evidence (evidence.cpp planIntrinsic): every way of the method cache held
// that native (a harvest names it `intrinsic:Math.<name>`), and the call
// passes exactly the arguments the computed form takes.
//
//     guard is.number %a ; x = unbox.f64.unchecked %a     (per argument;
//                                     imul: t = to.int32 x)
//     m = prop.get %recv, <key>, <a fresh IC>              ; the generic read
//     br (fn.native_is m, <intrinsic>), fast, exit
//   fast:  s = math.<intrinsic> x[, y] ; r = box s
//   exit:  t = call.dynamic m, %recv, a0, ...             ; finishes the op
//          leave @G, <point AFTER the site>, state with r := t
//
// The argument tests come first and leave IN FRONT of the site: they are
// pure, and nothing has happened yet. The read is the method call's own
// GetValue, so once it has run a failing identity test cannot undo it (a
// getter installed since would run again in G); that edge finishes the
// generic call with the value read, exactly as method_call.cpp's MethodProto
// does, and resumes G after the call.
//
// Why exact: a function object whose code is the intrinsic's native IS that
// Math function (natives keep no state the code word does not name), and on
// Number arguments it computes what `math.*` computes: the same kernel
// (il2mir il_lowering_spec.cpp lower_math_fn) or the machine instruction the
// IEEE operation is (sqrt, fabs, floor, ceil), and `imul` the wrapping
// product of the two ToInt32s. The receiver is not tested: the read is
// generic and the identity is what licenses the computed form.
//
// PINNED. When the receiver is `global.get "Math"` and the module's Math is
// pinned (il::Module::mathPinned: lockdown froze Math and made the binding
// read-only), `Math.<key>` can only be the intrinsic, so the read and the
// identity test go and no harvest is needed: only the argument tests stay,
// leaving in front of the site. The same holds for a call of a value read
// from the pinned Math (`const cos = Math.cos; cos(x)`), which then takes
// this form instead of the cold generic arm below, and its result stays a
// double instead of a Dynamic joined with the generic call's.
//
// A plain call `r = call.dynamic %f, %this, a0[, a1]` of a Math function held
// in a variable (`const cos = Math.cos; cos(x)`), whose harvest names the one
// callee the site ever held `intrinsic:Math.<name>`, has nothing to undo:
// the callee is a value the site was handed. Every test that fails makes the
// call it is, in a cold arm, and S goes on:
//
//     br (is.number %a), next, generic      ; per Dynamic argument
//   next: x = unbox.f64.unchecked %a ...
//     br (fn.native_is %f, <intrinsic>), fast, generic
//   fast:    s = math.<intrinsic> x[, y] ; jump join(box s)
//   generic: t = call.dynamic %f, %this, a0, ... ; jump join(t)   (cold)

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

namespace {

// The plain call's form (above): no exit, a cold generic arm.
void rewriteMathValueCall(RewriteContext& ctx, GuardBuilder& b, const SitePlan& plan) {
    const il::Instruction& site = b.site();
    const std::vector<ValueId> operands = site.operands;
    const ValueId callee = operands[0];
    const std::vector<ValueId> args(operands.begin() + 2, operands.end());
    const bool imul = static_cast<il::Intrinsic>(plan.intrinsic) == il::Intrinsic::MathImul;
    const bool hasResult = site.result != il::kNoValue;

    const BlockId generic = b.newBlock();
    const BlockId join = b.newBlock();
    ctx.speculated.blocks[generic].cold = true;
    if (hasResult) b.resultParam(join);

    std::vector<ValueId> xs;
    for (ValueId a : args) {
        ValueId x = a;
        if (b.typeOf(a) == il::Type::Dynamic) {
            const BlockId next = b.newBlock();
            b.branch(b.emit(il::Op::IsNumber, il::Type::Bool, {a}), next, {}, generic, {});
            b.moveTo(next);
            x = b.emit(il::Op::UnboxF64, il::Type::F64, {a});
        } else if (b.typeOf(a) == il::Type::I32 && !imul) {
            x = b.emit(il::Op::I32ToF64, il::Type::F64, {a});
        }
        if (imul && b.typeOf(x) != il::Type::I32) {
            il::Instruction conv;
            conv.op = il::Op::ToInt32;
            conv.type = il::Type::I32;
            conv.boxType = b.typeOf(x);
            conv.operands = {x};
            x = b.emitInst(std::move(conv));
        }
        xs.push_back(x);
    }
    const BlockId fast = b.newBlock();
    b.branch(b.emit(il::Op::FnNativeIs, il::Type::Bool, {callee}, plan.intrinsic), fast, {}, generic, {});

    b.moveTo(fast);
    const il::Type type = imul ? il::Type::I32 : il::Type::F64;
    const ValueId r = b.emit(il::Op::MathFn, type, xs, plan.intrinsic);
    il::Instruction boxed;
    boxed.op = il::Op::Box;
    boxed.type = il::Type::Dynamic;
    boxed.boxType = type;
    boxed.operands = {r};
    const ValueId rb = b.emitInst(std::move(boxed));
    b.jump(join, hasResult ? std::vector<ValueId>{rb} : std::vector<ValueId>{});

    b.moveTo(generic);
    il::Instruction call;
    call.op = il::Op::DynamicCall;
    call.type = il::Type::Dynamic;
    call.operands = operands;
    const ValueId t = b.emitInst(std::move(call));
    b.jump(join, hasResult ? std::vector<ValueId>{t} : std::vector<ValueId>{});

    b.moveTo(join);
    b.finish();
    ++ctx.stats.mathCalls;
}

// The method form's read and identity test: the read is the call's own
// GetValue, and a failing test finishes the call generically with the value
// read and resumes G after it.
void testMethod(RewriteContext& ctx, GuardBuilder& b, const SitePlan& plan, ValueId receiver,
                const std::vector<ValueId>& args) {
    const il::Instruction& site = b.site();
    il::Instruction read;
    read.op = il::Op::PropGet;
    read.type = il::Type::Dynamic;
    read.operands = {receiver};
    read.keyIndex = site.keyIndex;
    read.icIndex = ctx.newIcSite();
    const ValueId method = b.emitInst(std::move(read));

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
    b.guardTo(b.emit(il::Op::FnNativeIs, il::Type::Bool, {method}, plan.intrinsic), true, exit);
}

}  // namespace

void rewriteMathCall(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::MathCall);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction& site = b.site();
    if (site.op == il::Op::DynamicCall && !plan.pinned) {
        rewriteMathValueCall(ctx, b, plan);
        return;
    }
    assert((site.op == il::Op::MethodCall || site.op == il::Op::DynamicCall) && site.type == il::Type::Dynamic);
    const bool value = site.op == il::Op::DynamicCall;
    const ValueId receiver = site.operands[value ? 1 : 0];
    const std::vector<ValueId> args(site.operands.begin() + (value ? 2 : 1), site.operands.end());
    assert(il::intrinsicInfo(plan.intrinsic) != nullptr && args.size() == il::intrinsicInfo(plan.intrinsic)->arity);
    const bool imul = static_cast<il::Intrinsic>(plan.intrinsic) == il::Intrinsic::MathImul;

    std::vector<ValueId> xs;
    for (ValueId a : args) {
        ValueId x = a;
        if (b.typeOf(a) == il::Type::Dynamic) {
            b.guard(b.emit(il::Op::IsNumber, il::Type::Bool, {a}));
            x = b.emit(il::Op::UnboxF64, il::Type::F64, {a});
        } else if (b.typeOf(a) == il::Type::I32 && !imul) {
            x = b.emit(il::Op::I32ToF64, il::Type::F64, {a});
        }
        if (imul && b.typeOf(x) != il::Type::I32) {
            il::Instruction conv;
            conv.op = il::Op::ToInt32;
            conv.type = il::Type::I32;
            conv.boxType = b.typeOf(x);
            conv.operands = {x};
            x = b.emitInst(std::move(conv));
        }
        xs.push_back(x);
    }

    if (!plan.pinned) testMethod(ctx, b, plan, receiver, args);

    const il::Type type = imul ? il::Type::I32 : il::Type::F64;
    const ValueId r = b.emit(il::Op::MathFn, type, xs, plan.intrinsic);
    il::Instruction boxed;
    boxed.op = il::Op::Box;
    boxed.type = il::Type::Dynamic;
    boxed.boxType = type;
    boxed.operands = {r};
    b.emitResult(std::move(boxed));
    b.finish();
    ++ctx.stats.mathCalls;
}

}  // namespace bronze::opt::speculate
