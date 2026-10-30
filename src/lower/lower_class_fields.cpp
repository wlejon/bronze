// Class fields: how the statement `buildFieldInitStatements` synthesizes for a
// public instance field is stored, and the store a static field makes on the
// constructor.
//
// A field is DEFINED (7.3.33 DefineField, 7.3.7 CreateDataPropertyOrThrow), not
// assigned. The two agree only where nothing on the receiver's prototype chain
// has the field's name as an accessor or a read-only property — and that chain
// runs through Object.prototype, which any module, any library or a frozen
// realm can have reshaped. So every field is a define (il.h kWriteDefine): the
// site keeps an inline cache of its own, filled only by the definition helper,
// and no inherited setter ever runs for it.

#include <string>

#include "lower/lowerer.h"

namespace bronze::lower {

// `this.k = v` for field `k`: a `prop.set` in define mode with a site of its
// own. No static-slot claim and no accessor site — both describe what an
// ASSIGNMENT finds on the chain, which a definition never asks.
void Lowerer::emitDefineWrite(Value target, const std::string& key, Value value, Span span,
                              il::Function& ilFn) {
    il::Instruction inst;
    inst.op = il::Op::PropSet;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {boxValueIfNeeded(target, ilFn).id, boxValueIfNeeded(value, ilFn).id};
    inst.keyIndex = getKeyConstantIndex(key);
    inst.icIndex = icSiteCounter_++;
    inst.immI32 = il::kWriteDefine;
    inst.span = span;
    emitInst(ilFn, inst);
}

// The same definition with a key that is a value (`[k] = v`); the runtime runs
// ToPropertyKey on it.
void Lowerer::emitDefineElemWrite(Value target, Value key, Value value, Span span,
                                  il::Function& ilFn) {
    il::Instruction inst;
    inst.op = il::Op::ElemSet;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {boxValueIfNeeded(target, ilFn).id, boxValueIfNeeded(key, ilFn).id,
                     boxValueIfNeeded(value, ilFn).id};
    inst.immI32 = il::kWriteDefine;
    inst.span = span;
    emitInst(ilFn, inst);
}

// A public field `k = init`: the receiver, then the initializer — which takes
// the field's name if it is an anonymous function (15.7.10 step 1.a's
// NamedEvaluation) — then the definition.
std::optional<Lowerer::Value> Lowerer::lowerFieldDefinition(const ast::MemberAccess& mem,
                                                            const ast::Expr& rhs,
                                                            il::Function& ilFn) {
    auto objVal = lowerExpr(*mem.object, ilFn);
    if (!objVal) return std::nullopt;
    const Value objBoxed = boxValueIfNeeded(*objVal, ilFn);
    auto rhsVal = lowerNamedEvaluation(rhs, mem.property, ilFn);
    if (!rhsVal) return std::nullopt;
    const Value valBoxed = boxValueIfNeeded(*rhsVal, ilFn);
    emitDefineWrite(objBoxed, mem.property, valBoxed, mem.span, ilFn);
    return valBoxed;
}

// A computed public field `[e] = init`. The key expression is evaluated before
// the initializer, as the statement it stands for reads.
std::optional<Lowerer::Value> Lowerer::lowerComputedFieldDefinition(const ast::IndexAccess& idx,
                                                                    const ast::Expr& rhs,
                                                                    il::Function& ilFn) {
    auto objVal = lowerExpr(*idx.object, ilFn);
    if (!objVal) return std::nullopt;
    const Value objBoxed = boxValueIfNeeded(*objVal, ilFn);
    auto keyVal = lowerExpr(*idx.index, ilFn);
    if (!keyVal) return std::nullopt;
    const Value keyBoxed = boxValueIfNeeded(*keyVal, ilFn);
    auto rhsVal = lowerExpr(rhs, ilFn);
    if (!rhsVal) return std::nullopt;
    const Value valBoxed = boxValueIfNeeded(*rhsVal, ilFn);
    emitDefineElemWrite(objBoxed, keyBoxed, valBoxed, idx.span, ilFn);
    return valBoxed;
}

}  // namespace bronze::lower
