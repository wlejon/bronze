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

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewriteMathCall(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::MathCall);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction& site = b.site();
    assert(site.op == il::Op::MethodCall && site.type == il::Type::Dynamic);
    const ValueId receiver = site.operands[0];
    const std::vector<ValueId> args(site.operands.begin() + 1, site.operands.end());
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
