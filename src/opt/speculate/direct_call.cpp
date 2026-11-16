// THE DIRECT CALL S makes once a `fn.code_is` guard has named the callee.
//
//     guard fn.code_is %callee, @entry(f)
//     e = fn.env %callee                    ; only when f needsEnv
//     r = call @entry(f)(e?, this?, a0, ..., a(n-1), undefined...)
//
// `entry(f)` is the function f's closures run (context.h EntryMap): f's own
// speculated copy when it has one, so the identity test compares against the
// code word every closure of f actually carries once entry is redirected, and
// the call enters the speculated code. A recursive call of G from G or S goes
// to S the same way.
//
// The operands are exactly the callee's parameter list (the verifier's rule
// for a direct call): `__env` is the record the closure captured — `fn.env` of
// the very closure the guard identified, so `callEnvHops` is 0 (that record,
// no parent links) — `__this` the receiver, then one per source parameter.
// policy.h directCallFits is what makes this the uniform path's exact
// equivalent: no `arguments`, no rest array, no conversions, no class
// constructor, nothing that reads new.target. The receiver goes in as it is,
// primitive or not, which is what the method cache's direct dispatch already
// does (runtime/rt_method_call.cpp): a function's code binds `this` itself.
//
// The result keeps the generic site's type, Dynamic: a callee returning
// Dynamic defines it directly, a void one evaluates to `undefined`, a typed
// one is boxed — what its wrapper does with the same return.
//
// The call records the resume point in G right AFTER the generic call
// (il.h `resumeAfter`; GuardBuilder::resumeAfterSite), whose state is G's
// live values there, the generic call's result among them when it is used
// later. Nothing in S leaves to it on this account; it is the inliner's
// (opt/inline): a guard of the callee's body inlined here finishes the call in
// the callee's G and then leaves for this point. The MethodProto exit already
// leaves to the same point (one point per location).

#include "opt/speculate/policy.h"
#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

ValueId emitCodeIs(GuardBuilder& b, ValueId calleeValue, uint32_t callee) {
    il::Instruction test;
    test.op = il::Op::FnCodeIs;
    test.type = il::Type::Bool;
    test.operands = {calleeValue};
    test.calleeIndex = b.context().entries.entryOf(callee);
    return b.emitInst(std::move(test));
}

namespace {

// The call, and the instruction that makes its value Dynamic; `asResult`
// makes that instruction define the site's result.
ValueId directCall(GuardBuilder& b, uint32_t callee, ValueId calleeValue, ValueId thisValue,
                   const std::vector<ValueId>& args, bool asResult) {
    RewriteContext& ctx = b.context();
    const il::Function& f = ctx.module.functions[callee];

    il::Instruction call;
    call.op = il::Op::Call;
    call.calleeIndex = ctx.entries.entryOf(callee);
    call.type = f.returnType;
    call.resumeAfter = b.resumeAfterSite();
    if (f.needsEnv) {
        // Legal on the `fn.code_is` edge the caller established.
        call.operands.push_back(b.emit(il::Op::FnEnv, il::Type::Dynamic, {calleeValue}));
        call.callEnvHops = 0;
    }
    if (f.needsThis) call.operands.push_back(thisValue);
    const size_t fixed = f.callerParamCount();
    ValueId undefined = il::kNoValue;
    for (size_t i = 0; i < fixed; ++i) {
        if (i < args.size()) {
            call.operands.push_back(args[i]);
            continue;
        }
        if (undefined == il::kNoValue) undefined = b.constUndefined();
        call.operands.push_back(undefined);
    }
    assert(call.operands.size() == f.params.size());

    auto value = [&](il::Instruction inst) {
        if (!asResult) return b.emitInst(std::move(inst));
        b.emitResult(std::move(inst));
        return b.site().result;
    };
    switch (f.returnType) {
        case il::Type::Dynamic:
            return value(std::move(call));
        case il::Type::Void: {
            b.emitInst(std::move(call));
            il::Instruction undef;
            undef.op = il::Op::ConstUndefined;
            undef.type = il::Type::Dynamic;
            return value(std::move(undef));
        }
        default: {
            const ValueId raw = b.emitInst(std::move(call));
            il::Instruction boxed;
            boxed.op = il::Op::Box;
            boxed.type = il::Type::Dynamic;
            boxed.boxType = f.returnType;
            boxed.operands = {raw};
            return value(std::move(boxed));
        }
    }
}

}  // namespace

void emitDirectCall(GuardBuilder& b, uint32_t callee, ValueId calleeValue, ValueId thisValue,
                    const std::vector<ValueId>& args) {
    assert(directCallFits(b.context().module.functions[callee], args.size()));
    assert(b.site().type == il::Type::Dynamic && b.site().result != il::kNoValue);
    directCall(b, callee, calleeValue, thisValue, args, true);
}

ValueId emitDirectCallValue(GuardBuilder& b, uint32_t callee, ValueId calleeValue, ValueId thisValue,
                            const std::vector<ValueId>& args) {
    return directCall(b, callee, calleeValue, thisValue, args, false);
}

}  // namespace bronze::opt::speculate
