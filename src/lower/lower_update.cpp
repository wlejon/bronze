// Update expressions — `++x`, `x--`, `o.k++`, `a[i]--` (ECMA-262 13.4).
//
// One production, three reference kinds, and a single shared shape: evaluate
// the reference ONCE, read it, ToNumeric the old value, add or subtract one,
// write it back through the same reference. The postfix form yields the old
// value and the prefix form the new one, and the difference between them is
// only which of the two values is returned — never a second read.
//
// It lives apart from `lower_expr.cpp` because the reference part is what
// makes it hard: `f().k++` may call `f` once and `a[i++]++` may increment `i`
// once, which is the same evaluation-order contract compound assignment signs
// (13.15.2) and nothing else in the unary family has.

#include <optional>
#include <string>

#include "lower/lowerer.h"

namespace bronze::lower {

namespace {
bool isIncrement(ast::UnaryOp op) {
    return op == ast::UnaryOp::PreInc || op == ast::UnaryOp::PostInc;
}
bool isPrefix(ast::UnaryOp op) {
    return op == ast::UnaryOp::PreInc || op == ast::UnaryOp::PreDec;
}
}  // namespace

// ToNumeric(oldValue) ± 1, as an f64. Shared by all three reference kinds so
// that `o.k = "5"; o.k++` and `let k = "5"; k++` cannot disagree about what
// the old value was: both are ToNumeric first, so both leave a NUMBER behind
// and both yield a NUMBER (13.4.2.1 step 2 — the coercion happens before the
// arithmetic, not as part of it).
Lowerer::Value Lowerer::emitUpdateStep(Value oldNumeric, ast::UnaryOp op, il::Function& ilFn) {
    il::ValueId oneRes = ilFn.valueCount++;
    il::Instruction oneInst;
    oneInst.op = il::Op::ConstF64;
    oneInst.type = il::Type::F64;
    oneInst.result = oneRes;
    oneInst.immF64 = 1.0;
    emitInst(ilFn, oneInst);

    il::ValueId newValId = ilFn.valueCount++;
    il::Instruction calcInst;
    calcInst.op = isIncrement(op) ? il::Op::Add : il::Op::Sub;
    calcInst.type = il::Type::F64;
    calcInst.result = newValId;
    calcInst.operands = {oldNumeric.id, oneRes};
    emitInst(ilFn, calcInst);
    return Value{newValId, il::Type::F64};
}

std::optional<Lowerer::Value> Lowerer::lowerUpdate(const ast::Unary& un, il::Function& ilFn) {
    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(un.operand.get())) {
        return lowerMemberUpdate(*mem, un.op, ilFn);
    }
    if (const auto* idx = dynamic_cast<const ast::IndexAccess*>(un.operand.get())) {
        return lowerIndexUpdate(*idx, un.op, ilFn);
    }
    const auto* ident = dynamic_cast<const ast::Ident*>(un.operand.get());
    if (!ident) {
        diags_.error(un.span, "invalid update operand: ++/-- needs a variable or a property");
        return std::nullopt;
    }
    // An update expression is a read and a write of the same target, so it
    // resolves the target exactly as assignment does: a binding of this
    // function, or — failing that — a slot in an enclosing scope's environment
    // record. Looking only in `activeVarMap_` made `() => ++n` report
    // `undefined variable: n` for the very binding `n = n + 1` two lines away
    // resolves through the environment.
    auto it = activeVarMap_.find(ident->name);
    const bool isLocal = it != activeVarMap_.end();
    // An index rather than a reference, for the reason the assignment
    // path records: nothing between here and the write may lower a
    // closure, but the two paths must not differ in a way that only
    // holds by accident.
    const size_t bindingIdx = isLocal ? it->second : 0;
    uint32_t depth = 0;
    uint32_t index = 0;
    if (!isLocal &&
        (currentEnvValue_ == il::kNoValue || !findEnclosingEnvVar(ident->name, depth, index))) {
        if (!resolvesName(ident->name)) {
            // `x++` on an unresolvable name throws at the READ, before
            // any write is attempted (13.4.4.1 step 1 is GetValue).
            return emitReferenceError(ident->name, ident->span, ilFn);
        }
        // A name the ladder DOES resolve, but not to something an
        // update can write: a module function, or a provided global.
        diags_.error(ident->span, "cannot assign to '" + ident->name + "'");
        return std::nullopt;
    }
    Value oldVal =
        isLocal ? readBinding(varBindings_[bindingIdx], ilFn) : emitEnvGet(depth, index, ilFn);
    Value numOld = unboxValueIfNeeded(oldVal, il::Type::F64, ilFn);
    Value newVal = emitUpdateStep(numOld, un.op, ilFn);

    if (isLocal) {
        writeBinding(varBindings_[bindingIdx], newVal, ilFn);
    } else {
        emitEnvSet(depth, index, newVal, ilFn, /*assigning=*/true);
    }
    return isPrefix(un.op) ? newVal : numOld;
}

// `o.k++`. The base is lowered once and both the read and the write name the
// same value, so an accessor pair sees the same receiver for its getter and its
// setter and `f().k++` calls `f` once.
std::optional<Lowerer::Value> Lowerer::lowerMemberUpdate(const ast::MemberAccess& mem,
                                                         ast::UnaryOp op, il::Function& ilFn) {
    auto objVal = lowerExpr(*mem.object, ilFn);
    if (!objVal) return std::nullopt;
    Value objBoxed = boxValueIfNeeded(*objVal, ilFn);
    uint32_t keyIdx = getKeyConstantIndex(mem.property);

    il::ValueId cur = ilFn.valueCount++;
    il::Instruction getInst;
    getInst.op = il::Op::PropGet;
    getInst.type = il::Type::Dynamic;
    getInst.result = cur;
    getInst.operands = {objBoxed.id};
    getInst.keyIndex = keyIdx;
    getInst.icIndex = icSiteCounter_++;
    getInst.icMonomorphic = monomorphicPropSite(*mem.object);
    emitInst(ilFn, getInst);

    Value numOld = unboxValueIfNeeded(Value{cur, il::Type::Dynamic}, il::Type::F64, ilFn);
    Value newVal = emitUpdateStep(numOld, op, ilFn);
    Value storedBoxed = boxValueIfNeeded(newVal, ilFn);

    il::Instruction setInst;
    setInst.op = il::Op::PropSet;
    setInst.type = il::Type::Void;
    setInst.result = il::kNoValue;
    setInst.operands = {objBoxed.id, storedBoxed.id};
    setInst.keyIndex = keyIdx;
    setInst.icIndex = icSiteCounter_++;
    emitInst(ilFn, setInst);

    return isPrefix(op) ? newVal : numOld;
}

// `a[i]++`. Both halves of the reference — base AND index — are evaluated
// once, which is the half of the contract a member update does not have:
// lowering the index twice would make `a[i++]++` increment `i` twice.
std::optional<Lowerer::Value> Lowerer::lowerIndexUpdate(const ast::IndexAccess& idxAccess,
                                                        ast::UnaryOp op, il::Function& ilFn) {
    auto objVal = lowerExpr(*idxAccess.object, ilFn);
    if (!objVal) return std::nullopt;
    Value objBoxed = boxValueIfNeeded(*objVal, ilFn);

    // A literal key is a property name known at compile time, so it takes the
    // same inline-cached path a member access does — the assignment path makes
    // the same choice, and the two must agree or `a[0]++` and `a[0] += 1`
    // would carry different caches for one site.
    const std::optional<uint32_t> literalKey = literalIndexKey(*idxAccess.index);
    std::optional<Value> idxBoxed;
    if (!literalKey) {
        auto indexVal = lowerExpr(*idxAccess.index, ilFn);
        if (!indexVal) return std::nullopt;
        idxBoxed = boxValueIfNeeded(*indexVal, ilFn);
    }

    il::ValueId cur = ilFn.valueCount++;
    il::Instruction getInst;
    if (literalKey) {
        getInst.op = il::Op::PropGet;
        getInst.operands = {objBoxed.id};
        getInst.keyIndex = *literalKey;
        getInst.icIndex = icSiteCounter_++;
        getInst.icMonomorphic = monomorphicPropSite(*idxAccess.object);
    } else {
        getInst.op = il::Op::ElemGet;
        getInst.operands = {objBoxed.id, idxBoxed->id};
    }
    getInst.type = il::Type::Dynamic;
    getInst.result = cur;
    emitInst(ilFn, getInst);

    Value numOld = unboxValueIfNeeded(Value{cur, il::Type::Dynamic}, il::Type::F64, ilFn);
    Value newVal = emitUpdateStep(numOld, op, ilFn);
    Value storedBoxed = boxValueIfNeeded(newVal, ilFn);

    il::Instruction setInst;
    if (literalKey) {
        setInst.op = il::Op::PropSet;
        setInst.operands = {objBoxed.id, storedBoxed.id};
        setInst.keyIndex = *literalKey;
        setInst.icIndex = icSiteCounter_++;
    } else {
        setInst.op = il::Op::ElemSet;
        setInst.operands = {objBoxed.id, idxBoxed->id, storedBoxed.id};
    }
    setInst.type = il::Type::Void;
    setInst.result = il::kNoValue;
    emitInst(ilFn, setInst);

    return isPrefix(op) ? newVal : numOld;
}

}  // namespace bronze::lower
