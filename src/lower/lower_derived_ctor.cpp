// A derived constructor's receiver binding, at the sites `planDerivedCtor`
// could not decide (derived_ctor_plan.h).
//
// The state is one slot of the constructor's environment record, under a
// name no source can spell: `undefined` until a `super()` returns, the bound
// receiver after. A slot and not an SSA value because an arrow made in the
// constructor reads and writes the same binding (13.3.7.1 BindThisValue
// reaches the constructor's environment from an arrow's `super()`), and an
// arrow reaches the constructor's locals only through its record. A
// constructor the plan decided entirely has no slot, and none of what is here
// is emitted for it.

#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

const char* const Lowerer::kSuperStateSlot = "%super";
const char* const Lowerer::kNewTargetSlot = "new.target";

// 9.4.5 GetNewTarget: the new.target of the nearest non-arrow function. In
// that function it is the call's own; in an arrow it is the copy the
// function put in its record on entry, because the arrow is called later,
// from anywhere, and what is current then is whoever called it.
Lowerer::Value Lowerer::lowerNewTargetValue(il::Function& ilFn) {
    uint32_t depth = 0;
    uint32_t index = 0;
    if (currentFunctionIsArrow_ && currentEnvValue_ != il::kNoValue &&
        findEnclosingEnvVar(kNewTargetSlot, depth, index)) {
        return emitEnvGet(depth, index, ilFn);
    }
    il::Instruction inst;
    inst.op = il::Op::GetNewTarget;
    inst.type = il::Type::Dynamic;
    inst.result = ilFn.valueCount++;
    emitInst(ilFn, inst);
    return Value{inst.result, il::Type::Dynamic};
}

// Calls a runtime helper that raises, then throws what it returned — which
// it never does, since it raises; the `throw` is the block's terminator, so
// that the block ends the way the verifier needs every block to.
void Lowerer::emitRaisingHelper(const char* symbol, const std::vector<il::ValueId>& args,
                                Span span, il::Function& ilFn) {
    std::vector<il::Type> params(args.size(), il::Type::Dynamic);
    const uint32_t fnIdx = registerExternalFunction(symbol, il::Type::Dynamic, params);
    const il::ValueId res = ilFn.valueCount++;
    il::Instruction call;
    call.op = il::Op::Call;
    call.type = il::Type::Dynamic;
    call.result = res;
    call.operands = args;
    call.calleeIndex = fnIdx;
    call.span = span;
    emitInst(ilFn, call);
    il::Instruction thr;
    thr.op = il::Op::Throw;
    thr.type = il::Type::Void;
    thr.result = il::kNoValue;
    thr.operands = {res};
    thr.span = span;
    emitInst(ilFn, thr);
}

// The state slot's value, read where the plan says a test goes: from the
// constructor's own body or from an arrow inside it.
std::optional<Lowerer::Value> Lowerer::readSuperState(Span span, il::Function& ilFn) {
    uint32_t depth = 0;
    uint32_t index = 0;
    if (currentEnvValue_ == il::kNoValue || !findEnclosingEnvVar(kSuperStateSlot, depth, index)) {
        diags_.error(span, "internal: a derived constructor's binding test has no state slot");
        return std::nullopt;
    }
    return emitEnvGet(depth, index, ilFn);
}

// Whether the state still says no `super()` has returned, as a Bool: the
// slot holds `undefined` until the first one stores the receiver over it.
std::optional<il::ValueId> Lowerer::emitSuperStateIsUnbound(Span span, il::Function& ilFn) {
    auto state = readSuperState(span, ilFn);
    if (!state) return std::nullopt;
    const il::ValueId unbound = ilFn.valueCount++;
    il::Instruction test;
    test.op = il::Op::IsNullish;
    test.type = il::Type::Bool;
    test.result = unbound;
    test.operands = {state->id};
    emitInst(ilFn, test);
    return unbound;
}

void Lowerer::emitBranchTo(il::ValueId cond, il::BlockId then, il::BlockId otherwise,
                           il::Function& ilFn) {
    il::Instruction br;
    br.op = il::Op::Branch;
    br.type = il::Type::Void;
    br.result = il::kNoValue;
    br.operands = {cond};
    br.target = il::BlockTarget{.block = then, .args = {}};
    br.elseTarget = il::BlockTarget{.block = otherwise, .args = {}};
    emitInst(ilFn, br);
}

// 9.1.1.3.4 GetThisBinding: a ReferenceError while the receiver is unbound.
// Nothing at all where the plan proved it bound, which is every site of a
// straight-line constructor.
bool Lowerer::emitThisBindingCheck(const void* site, Span span, il::Function& ilFn) {
    if (derivedPlan_ == nullptr || !derivedPlan_->isChecked(site)) return true;
    return emitThisUnboundTest(span, ilFn);
}

bool Lowerer::emitThisUnboundTest(Span span, il::Function& ilFn) {
    auto unbound = emitSuperStateIsUnbound(span, ilFn);
    if (!unbound) return false;
    const il::BlockId bThrow = createBlock(ilFn);
    const il::BlockId bOk = createBlock(ilFn);
    emitBranchTo(*unbound, bThrow, bOk, ilFn);
    setCurrentBlock(bThrow);
    emitRaisingHelper("bronze_this_unbound", {}, span, ilFn);
    setCurrentBlock(bOk);
    return true;
}

// A `super()` the plan could not prove the first: the base is constructed
// either way (13.3.7.1 step 6), so the arguments are evaluated once, as a
// list, and the state decides which construction it is — the ordinary one
// on the instance `new` allocated, or a fresh one the helper makes and throws
// after (step 7's BindThisValue on a bound receiver).
std::optional<Lowerer::Value> Lowerer::lowerCheckedSuperCall(const ast::SuperCall& sc, Value base,
                                                             Value receiver, il::Function& ilFn) {
    auto argsArr = lowerListToArray(sc.args, ilFn);
    if (!argsArr) return std::nullopt;
    auto unbound = emitSuperStateIsUnbound(sc.span, ilFn);
    if (!unbound) return std::nullopt;
    const il::BlockId bFirst = createBlock(ilFn);
    const il::BlockId bAgain = createBlock(ilFn);
    const Value newTarget = lowerNewTargetValue(ilFn);
    emitBranchTo(*unbound, bFirst, bAgain, ilFn);

    setCurrentBlock(bAgain);
    emitRaisingHelper("bronze_super_rebind", {base.id, argsArr->id, newTarget.id}, sc.span, ilFn);

    setCurrentBlock(bFirst);
    const il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.span = sc.span;
    if (currentFunctionIsArrow_) {
        // From an arrow, the base runs under the constructor's new.target,
        // not the arrow call's (which is undefined), so the helper puts it
        // back for the construction.
        inst.op = il::Op::Call;
        inst.calleeIndex = registerExternalFunction(
            "bronze_super_call_lexical", il::Type::Dynamic,
            {il::Type::Dynamic, il::Type::Dynamic, il::Type::Dynamic, il::Type::Dynamic});
        inst.operands = {base.id, receiver.id, argsArr->id, newTarget.id};
    } else {
        inst.op = il::Op::SuperCallSpread;
        inst.operands = {base.id, receiver.id, argsArr->id};
    }
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

// After every `super()` of a constructor that has the state: the receiver is
// bound from here on, for this body and for every arrow in it.
bool Lowerer::markSuperCalled(Value bound, Span span, il::Function& ilFn) {
    if (derivedPlan_ == nullptr || !derivedPlan_->needsState()) return true;
    uint32_t depth = 0;
    uint32_t index = 0;
    if (currentEnvValue_ == il::kNoValue || !findEnclosingEnvVar(kSuperStateSlot, depth, index)) {
        diags_.error(span, "internal: a derived constructor's super() has no state slot");
        return false;
    }
    emitEnvSet(depth, index, bound, ilFn);
    return true;
}

// The field initializers of a class whose constructor does not call `super()`
// exactly once as a statement of its own body: run where each call returns
// (15.7.14's InitializeInstanceElements is step 8 of SuperCall evaluation, not
// a fixed point of the constructor), from whichever function the call is in.
bool Lowerer::emitFieldsAfterSuper(il::Function& ilFn) {
    if (derivedFieldStmts_ == nullptr) return true;
    std::vector<const ast::Stmt*> stmts;
    stmts.reserve(derivedFieldStmts_->size());
    for (const auto& s : *derivedFieldStmts_) stmts.push_back(s.get());
    return lowerStmtList(stmts, ilFn);
}

}  // namespace bronze::lower
