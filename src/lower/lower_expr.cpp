// Expression lowering: the dispatcher, literals, identifier resolution,
// unary operators, the binary operators and assignment. The forms with
// their own seams delegate to lower_expr_cond.cpp (short-circuit joins)
// and lower_object.cpp (objects, property access, new, calls).

#include <limits>
#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

std::optional<Lowerer::Value> Lowerer::lowerExpr(const ast::Expr& expr, il::Function& ilFn) {
    if (const auto* numLit = dynamic_cast<const ast::NumberLit*>(&expr)) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::ConstF64;
        inst.type = il::Type::F64;
        inst.result = res;
        inst.immF64 = numLit->value;
        emitInst(ilFn, inst);
        return Value{res, il::Type::F64};
    }

    if (const auto* boolLit = dynamic_cast<const ast::BoolLit*>(&expr)) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::ConstBool;
        inst.type = il::Type::Bool;
        inst.result = res;
        inst.immI32 = boolLit->value ? 1 : 0;
        emitInst(ilFn, inst);
        return Value{res, il::Type::Bool};
    }

    if (dynamic_cast<const ast::NullLit*>(&expr)) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::ConstNull;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }

    if (dynamic_cast<const ast::UndefinedLit*>(&expr)) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::ConstUndefined;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }

    if (dynamic_cast<const ast::ThisExpr*>(&expr)) {
        return lowerThisValue(expr.span, ilFn);
    }

    if (const auto* superCall = dynamic_cast<const ast::SuperCall*>(&expr)) {
        return lowerSuperCall(superCall, ilFn);
    }

    if (const auto* superMem = dynamic_cast<const ast::SuperMember*>(&expr)) {
        return lowerSuperMember(superMem, ilFn);
    }

    if (const auto* strLit = dynamic_cast<const ast::StringLit*>(&expr)) {
        uint32_t keyIdx = getKeyConstantIndex(strLit->value);
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::Box;
        inst.type = il::Type::Dynamic;
        inst.boxType = il::Type::Str;
        inst.result = res;
        inst.keyIndex = keyIdx;
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }

    // A template is left-to-right concatenation starting from its first
    // piece. `add.dynamic` is exactly the operation the language specifies —
    // ToString of whatever the substitution produced, because the left side
    // is already a string — so no separate ToString op is needed.
    //
    // The leading piece is emitted even when it is empty, and that is not a
    // missed optimization: `${a}${b}` without it would be `a + b`, which for
    // two numbers is addition rather than concatenation. Every LATER empty
    // piece is skipped, because by then the accumulator is a string and the
    // operation cannot change meaning.
    if (const auto* tmpl = dynamic_cast<const ast::TemplateLit*>(&expr)) {
        auto emitStr = [&](const std::string& text) {
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::Box;
            inst.type = il::Type::Dynamic;
            inst.boxType = il::Type::Str;
            inst.result = res;
            inst.keyIndex = getKeyConstantIndex(text);
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        };
        auto emitConcat = [&](Value lhs, Value rhs) {
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::Add;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            inst.operands = {lhs.id, rhs.id};
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        };

        Value acc = emitStr(tmpl->quasis.empty() ? std::string() : tmpl->quasis[0]);
        for (size_t i = 0; i < tmpl->exprs.size(); ++i) {
            auto sub = lowerExpr(*tmpl->exprs[i], ilFn);
            if (!sub) return std::nullopt;
            acc = emitConcat(acc, boxValueIfNeeded(*sub, ilFn));
            if (i + 1 < tmpl->quasis.size() && !tmpl->quasis[i + 1].empty()) {
                acc = emitConcat(acc, emitStr(tmpl->quasis[i + 1]));
            }
        }
        return acc;
    }

    if (const auto* objLit = dynamic_cast<const ast::ObjectLit*>(&expr)) {
        return lowerObjectLit(objLit, ilFn);
    }

    if (const auto* arrLit = dynamic_cast<const ast::ArrayLit*>(&expr)) {
        return lowerArrayLit(arrLit, ilFn);
    }

    if (const auto* fnExpr = dynamic_cast<const ast::FunctionExpr*>(&expr)) {
        return lowerClosure(*fnExpr, fnExpr->name, fnExpr->params, fnExpr->returnType,
                            fnExpr->body, fnExpr->span, ilFn, fnExpr->isArrow);
    }

    if (const auto* ident = dynamic_cast<const ast::Ident*>(&expr)) {
        auto it = activeVarMap_.find(ident->name);
        if (it == activeVarMap_.end()) {
            // Global value properties (shadowable by local declarations,
            // hence checked only after the variable lookup misses).
            if (ident->name == "NaN" || ident->name == "Infinity") {
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::ConstF64;
                inst.type = il::Type::F64;
                inst.result = res;
                inst.immF64 = ident->name == "NaN"
                                  ? std::numeric_limits<double>::quiet_NaN()
                                  : std::numeric_limits<double>::infinity();
                emitInst(ilFn, inst);
                return Value{res, il::Type::F64};
            }
            if (ident->name == "undefined") {
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::ConstUndefined;
                inst.type = il::Type::Dynamic;
                inst.result = res;
                emitInst(ilFn, inst);
                return Value{res, il::Type::Dynamic};
            }
            // A free variable of a nested function: resolved against
            // the environments of the enclosing scopes (docs/0007).
            uint32_t depth = 0;
            uint32_t index = 0;
            if (currentEnvValue_ != il::kNoValue &&
                findEnclosingEnvVar(ident->name, depth, index)) {
                return emitEnvGet(depth, index, ilFn);
            }
            // A top-level function declaration used as a value rather
            // than called: `new Point(...)`, `Point.prototype`, passing
            // it around. One object per declaration, so decorating it
            // in one place is visible in every other (docs/0008).
            auto fnIt = functionIndices_.find(ident->name);
            if (fnIt != functionIndices_.end()) {
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::FunctionRef;
                inst.type = il::Type::Dynamic;
                inst.result = res;
                inst.calleeIndex = fnIt->second;
                emitInst(ilFn, inst);
                return Value{res, il::Type::Dynamic};
            }
            // A global bronze provides. The set is closed and checked
            // HERE, at compile time, so an unknown free identifier is a
            // compile error rather than a runtime miss a program could
            // feature-test its way around (docs/0011 decision 1).
            if (isProvidedGlobal(ident->name)) {
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::GlobalGet;
                inst.type = il::Type::Dynamic;
                inst.result = res;
                inst.keyIndex = getKeyConstantIndex(ident->name);
                emitInst(ilFn, inst);
                return Value{res, il::Type::Dynamic};
            }
            diags_.error(ident->span, "undefined variable: " + ident->name);
            return std::nullopt;
        }
        const auto& b = varBindings_[it->second];
        if (!b.isInitialized) {
            if (b.isConst) {
                diags_.error(ident->span, "use of 'const' binding before initialization");
            } else {
                diags_.error(ident->span, "use of 'let' binding before initialization");
            }
            return std::nullopt;
        }
        return readBinding(b, ilFn);
    }

    if (const auto* un = dynamic_cast<const ast::Unary*>(&expr)) {
        if (un->op == ast::UnaryOp::Not) {
            Value condVal = lowerCondition(*un->operand, ilFn);
            il::ValueId falseVal = ilFn.valueCount++;
            il::Instruction falseInst;
            falseInst.op = il::Op::ConstBool;
            falseInst.type = il::Type::Bool;
            falseInst.result = falseVal;
            falseInst.immI32 = 0;
            emitInst(ilFn, falseInst);

            il::ValueId res = ilFn.valueCount++;
            il::Instruction cmpInst;
            cmpInst.op = il::Op::CmpEq;
            cmpInst.type = il::Type::Bool;
            cmpInst.result = res;
            cmpInst.operands = {condVal.id, falseVal};
            emitInst(ilFn, cmpInst);
            return Value{res, il::Type::Bool};
        }
        if (un->op == ast::UnaryOp::Negate) {
            auto valOpt = lowerExpr(*un->operand, ilFn);
            if (!valOpt) return std::nullopt;
            Value val = unboxValueIfNeeded(*valOpt, il::Type::F64, ilFn);
            // Negation, not `0 - x`: IEEE-754 makes 0 - 0 positive zero, so
            // the subtraction spelling printed `-0` as `0` — the sign of
            // zero is observable (docs/0013), and `Object.is(-0, 0)` is
            // false in the language.
            il::ValueId res = ilFn.valueCount++;
            il::Instruction negInst;
            negInst.op = il::Op::Neg;
            negInst.type = il::Type::F64;
            negInst.result = res;
            negInst.operands = {val.id};
            emitInst(ilFn, negInst);
            return Value{res, il::Type::F64};
        }
        if (un->op == ast::UnaryOp::Posate) {
            auto valOpt = lowerExpr(*un->operand, ilFn);
            if (!valOpt) return std::nullopt;
            return unboxValueIfNeeded(*valOpt, il::Type::F64, ilFn);
        }
        if (un->op == ast::UnaryOp::PreInc || un->op == ast::UnaryOp::PreDec ||
            un->op == ast::UnaryOp::PostInc || un->op == ast::UnaryOp::PostDec) {
            const auto* ident = dynamic_cast<const ast::Ident*>(un->operand.get());
            if (!ident) {
                diags_.error(un->span, "invalid update operand");
                return std::nullopt;
            }
            auto it = activeVarMap_.find(ident->name);
            if (it == activeVarMap_.end()) {
                diags_.error(ident->span, "undefined variable: " + ident->name);
                return std::nullopt;
            }
            VarBinding& b = varBindings_[it->second];
            Value oldVal = readBinding(b, ilFn);
            Value numOld = unboxValueIfNeeded(oldVal, il::Type::F64, ilFn);

            il::ValueId oneRes = ilFn.valueCount++;
            il::Instruction oneInst;
            oneInst.op = il::Op::ConstF64;
            oneInst.type = il::Type::F64;
            oneInst.result = oneRes;
            oneInst.immF64 = 1.0;
            emitInst(ilFn, oneInst);

            il::ValueId newValId = ilFn.valueCount++;
            il::Instruction calcInst;
            calcInst.op = (un->op == ast::UnaryOp::PreInc || un->op == ast::UnaryOp::PostInc) ? il::Op::Add : il::Op::Sub;
            calcInst.type = il::Type::F64;
            calcInst.result = newValId;
            calcInst.operands = {numOld.id, oneRes};
            emitInst(ilFn, calcInst);

            writeBinding(b, Value{newValId, il::Type::F64}, ilFn);

            if (un->op == ast::UnaryOp::PreInc || un->op == ast::UnaryOp::PreDec) {
                return Value{newValId, il::Type::F64};
            } else {
                return numOld;
            }
        }
        // Every `UnaryOp` above is handled, so this is unreachable today —
        // and that is exactly why it is here. Falling out of this `if` drops
        // into the dispatcher's generic "unsupported AST expression" at the
        // bottom, which names nothing; the house rule is that an unimplemented
        // construct is diagnosed BY NAME, and the next operator added to the
        // enum must not silently inherit the anonymous error.
        diags_.error(un->span, "unsupported unary operator: " +
                                   std::string(ast::unaryOpName(un->op)));
        return std::nullopt;
    }

    if (const auto* tern = dynamic_cast<const ast::Ternary*>(&expr)) {
        return lowerTernary(tern, ilFn);
    }

    if (const auto* newExpr = dynamic_cast<const ast::NewExpr*>(&expr)) {
        return lowerNewExpr(newExpr, ilFn);
    }

    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(&expr)) {
        return lowerMemberAccess(mem, ilFn);
    }

    if (const auto* idxAccess = dynamic_cast<const ast::IndexAccess*>(&expr)) {
        return lowerIndexAccess(idxAccess, ilFn);
    }

    if (const auto* bin = dynamic_cast<const ast::Binary*>(&expr)) {
        return lowerBinary(bin, ilFn);
    }

    if (const auto* call = dynamic_cast<const ast::Call*>(&expr)) {
        return lowerCall(call, ilFn);
    }

    diags_.error(expr.span, "unsupported AST expression");
    return std::nullopt;
}

std::optional<Lowerer::Value> Lowerer::lowerBinary(const ast::Binary* bin, il::Function& ilFn) {
    if (bin->op == ast::BinaryOp::Assign || bin->op == ast::BinaryOp::PlusAssign ||
        bin->op == ast::BinaryOp::MinusAssign || bin->op == ast::BinaryOp::StarAssign ||
        bin->op == ast::BinaryOp::SlashAssign || bin->op == ast::BinaryOp::PercentAssign) {
        return lowerAssignment(bin, ilFn);
    }

    if (bin->op == ast::BinaryOp::LogicalAnd || bin->op == ast::BinaryOp::LogicalOr) {
        return lowerLogical(bin, ilFn);
    }

    if (bin->op == ast::BinaryOp::NullishCoalescing) {
        return lowerNullish(bin, ilFn);
    }

    auto lhsOpt = lowerExpr(*bin->lhs, ilFn);
    if (!lhsOpt) return std::nullopt;
    auto rhsOpt = lowerExpr(*bin->rhs, ilFn);
    if (!rhsOpt) return std::nullopt;

    Value lhs = *lhsOpt;
    Value rhs = *rhsOpt;

    il::Op op;
    il::Type resType;

    switch (bin->op) {
        case ast::BinaryOp::Add:
            if (lhs.type == il::Type::Dynamic || rhs.type == il::Type::Dynamic ||
                lhs.type == il::Type::Str || rhs.type == il::Type::Str) {
                auto lhsBoxed = boxValueIfNeeded(lhs, ilFn);
                auto rhsBoxed = boxValueIfNeeded(rhs, ilFn);
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::Add;
                inst.type = il::Type::Dynamic;
                inst.result = res;
                inst.operands = {lhsBoxed.id, rhsBoxed.id};
                emitInst(ilFn, inst);
                return Value{res, il::Type::Dynamic};
            }
            op = il::Op::Add;
            resType = il::Type::F64;
            break;
        case ast::BinaryOp::Sub:
            op = il::Op::Sub;
            resType = il::Type::F64;
            break;
        case ast::BinaryOp::Mul:
            op = il::Op::Mul;
            resType = il::Type::F64;
            break;
        case ast::BinaryOp::Div:
            op = il::Op::Div;
            resType = il::Type::F64;
            break;
        case ast::BinaryOp::Mod:
            op = il::Op::Mod;
            resType = il::Type::F64;
            break;
        case ast::BinaryOp::Less:
            op = il::Op::CmpLt;
            resType = il::Type::Bool;
            break;
        case ast::BinaryOp::Greater:
            op = il::Op::CmpGt;
            resType = il::Type::Bool;
            break;
        case ast::BinaryOp::LessEqual: {
            // a <= b is !(a > b) -> cmp.gt, then cmp.eq false
            Value l = unboxValueIfNeeded(lhs, il::Type::F64, ilFn);
            Value r = unboxValueIfNeeded(rhs, il::Type::F64, ilFn);
            il::ValueId gtRes = ilFn.valueCount++;
            il::Instruction gtInst;
            gtInst.op = il::Op::CmpGt;
            gtInst.type = il::Type::Bool;
            gtInst.result = gtRes;
            gtInst.operands = {l.id, r.id};
            emitInst(ilFn, gtInst);

            il::ValueId falseVal = ilFn.valueCount++;
            il::Instruction falseInst;
            falseInst.op = il::Op::ConstBool;
            falseInst.type = il::Type::Bool;
            falseInst.result = falseVal;
            falseInst.immI32 = 0;
            emitInst(ilFn, falseInst);

            il::ValueId res = ilFn.valueCount++;
            il::Instruction cmpInst;
            cmpInst.op = il::Op::CmpEq;
            cmpInst.type = il::Type::Bool;
            cmpInst.result = res;
            cmpInst.operands = {gtRes, falseVal};
            emitInst(ilFn, cmpInst);
            return Value{res, il::Type::Bool};
        }
        case ast::BinaryOp::GreaterEqual: {
            // a >= b is !(a < b)
            Value l = unboxValueIfNeeded(lhs, il::Type::F64, ilFn);
            Value r = unboxValueIfNeeded(rhs, il::Type::F64, ilFn);
            il::ValueId ltRes = ilFn.valueCount++;
            il::Instruction ltInst;
            ltInst.op = il::Op::CmpLt;
            ltInst.type = il::Type::Bool;
            ltInst.result = ltRes;
            ltInst.operands = {l.id, r.id};
            emitInst(ilFn, ltInst);

            il::ValueId falseVal = ilFn.valueCount++;
            il::Instruction falseInst;
            falseInst.op = il::Op::ConstBool;
            falseInst.type = il::Type::Bool;
            falseInst.result = falseVal;
            falseInst.immI32 = 0;
            emitInst(ilFn, falseInst);

            il::ValueId res = ilFn.valueCount++;
            il::Instruction cmpInst;
            cmpInst.op = il::Op::CmpEq;
            cmpInst.type = il::Type::Bool;
            cmpInst.result = res;
            cmpInst.operands = {ltRes, falseVal};
            emitInst(ilFn, cmpInst);
            return Value{res, il::Type::Bool};
        }
        case ast::BinaryOp::Eq:
        case ast::BinaryOp::StrictEq:
        case ast::BinaryOp::Ne:
        case ast::BinaryOp::StrictNe: {
            bool negate = (bin->op == ast::BinaryOp::Ne || bin->op == ast::BinaryOp::StrictNe);
            bool loose = (bin->op == ast::BinaryOp::Eq || bin->op == ast::BinaryOp::Ne);

            Value eqRes{il::kNoValue, il::Type::Bool};
            if (lhs.type == il::Type::Dynamic || rhs.type == il::Type::Dynamic ||
                lhs.type == il::Type::Str || rhs.type == il::Type::Str) {
                if (loose) {
                    diags_.error(bin->span,
                                 "unsupported construct: loose equality (==, !=) on "
                                 "possibly non-numeric operands; use === / !==");
                    return std::nullopt;
                }
                Value lb = boxValueIfNeeded(lhs, ilFn);
                Value rb = boxValueIfNeeded(rhs, ilFn);
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::StrictEq;
                inst.type = il::Type::Bool;
                inst.result = res;
                inst.operands = {lb.id, rb.id};
                emitInst(ilFn, inst);
                eqRes = Value{res, il::Type::Bool};
            } else if (lhs.type != rhs.type) {
                if (loose) {
                    diags_.error(bin->span,
                                 "unsupported construct: loose equality (==, !=) on "
                                 "mixed primitive types; use === / !==");
                    return std::nullopt;
                }
                if (lhs.type == il::Type::I32 || rhs.type == il::Type::I32) {
                    diags_.error(bin->span,
                                 "unsupported construct: mixed i32/f64 strict equality");
                    return std::nullopt;
                }
                // Strict equality across distinct primitive types is
                // statically false.
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::ConstBool;
                inst.type = il::Type::Bool;
                inst.result = res;
                inst.immI32 = 0;
                emitInst(ilFn, inst);
                eqRes = Value{res, il::Type::Bool};
            } else {
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = negate ? il::Op::CmpNe : il::Op::CmpEq;
                inst.type = il::Type::Bool;
                inst.result = res;
                inst.operands = {lhs.id, rhs.id};
                emitInst(ilFn, inst);
                eqRes = Value{res, il::Type::Bool};
                negate = false;
            }

            if (negate) {
                il::ValueId falseVal = ilFn.valueCount++;
                il::Instruction falseInst;
                falseInst.op = il::Op::ConstBool;
                falseInst.type = il::Type::Bool;
                falseInst.result = falseVal;
                falseInst.immI32 = 0;
                emitInst(ilFn, falseInst);

                il::ValueId notRes = ilFn.valueCount++;
                il::Instruction notInst;
                notInst.op = il::Op::CmpEq;
                notInst.type = il::Type::Bool;
                notInst.result = notRes;
                notInst.operands = {eqRes.id, falseVal};
                emitInst(ilFn, notInst);
                eqRes = Value{notRes, il::Type::Bool};
            }
            return eqRes;
        }
        default:
            diags_.error(bin->span, "unsupported binary operator: " + std::string(ast::binaryOpName(bin->op)));
            return std::nullopt;
    }

    // Arithmetic and relational comparison are numeric: dynamic
    // operands go through runtime-checked ToNumber first.
    if (resType == il::Type::F64 || op == il::Op::CmpLt || op == il::Op::CmpGt) {
        lhs = unboxValueIfNeeded(lhs, il::Type::F64, ilFn);
        rhs = unboxValueIfNeeded(rhs, il::Type::F64, ilFn);
    }

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = op;
    inst.type = resType;
    inst.result = res;
    inst.operands = {lhs.id, rhs.id};
    emitInst(ilFn, inst);
    return Value{res, resType};
}

std::optional<Lowerer::Value> Lowerer::lowerAssignment(const ast::Binary* bin,
                                                       il::Function& ilFn) {
    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(bin->lhs.get())) {
        auto objVal = lowerExpr(*mem->object, ilFn);
        if (!objVal) return std::nullopt;
        auto objBoxed = boxValueIfNeeded(*objVal, ilFn);
        uint32_t keyIdx = getKeyConstantIndex(mem->property);

        // Compound assignment reads the current value before the
        // rhs is evaluated (JS evaluation order).
        std::optional<Value> curVal;
        if (bin->op != ast::BinaryOp::Assign) {
            il::ValueId cur = ilFn.valueCount++;
            il::Instruction getInst;
            getInst.op = il::Op::PropGet;
            getInst.type = il::Type::Dynamic;
            getInst.result = cur;
            getInst.operands = {objBoxed.id};
            getInst.keyIndex = keyIdx;
            getInst.icIndex = icSiteCounter_++;
            getInst.icMonomorphic = monomorphicPropSite(*mem->object);
            emitInst(ilFn, getInst);
            curVal = Value{cur, il::Type::Dynamic};
        }

        auto rhsVal = lowerExpr(*bin->rhs, ilFn);
        if (!rhsVal) return std::nullopt;
        Value stored = curVal ? emitCompoundCombine(*curVal, *rhsVal, bin->op,
                                                   provenNumber(*bin), ilFn)
                              : *rhsVal;
        Value storedBoxed = boxValueIfNeeded(stored, ilFn);

        il::Instruction inst;
        inst.op = il::Op::PropSet;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.operands = {objBoxed.id, storedBoxed.id};
        inst.keyIndex = keyIdx;
        inst.icIndex = icSiteCounter_++;
        emitInst(ilFn, inst);
        return storedBoxed;
    }
    if (const auto* idxAccess = dynamic_cast<const ast::IndexAccess*>(bin->lhs.get())) {
        auto objVal = lowerExpr(*idxAccess->object, ilFn);
        if (!objVal) return std::nullopt;
        auto objBoxed = boxValueIfNeeded(*objVal, ilFn);

        const std::optional<uint32_t> literalKey = literalIndexKey(*idxAccess->index);

        std::optional<Value> idxBoxed;
        if (!literalKey) {
            auto indexVal = lowerExpr(*idxAccess->index, ilFn);
            if (!indexVal) return std::nullopt;
            idxBoxed = boxValueIfNeeded(*indexVal, ilFn);
        }

        // Compound assignment reads the current element before
        // the rhs is evaluated (JS evaluation order).
        std::optional<Value> curVal;
        if (bin->op != ast::BinaryOp::Assign) {
            il::ValueId cur = ilFn.valueCount++;
            il::Instruction getInst;
            if (literalKey) {
                getInst.op = il::Op::PropGet;
                getInst.operands = {objBoxed.id};
                getInst.keyIndex = *literalKey;
                getInst.icIndex = icSiteCounter_++;
                getInst.icMonomorphic = monomorphicPropSite(*idxAccess->object);
            } else {
                getInst.op = il::Op::ElemGet;
                getInst.operands = {objBoxed.id, idxBoxed->id};
            }
            getInst.type = il::Type::Dynamic;
            getInst.result = cur;
            emitInst(ilFn, getInst);
            curVal = Value{cur, il::Type::Dynamic};
        }

        auto rhsVal = lowerExpr(*bin->rhs, ilFn);
        if (!rhsVal) return std::nullopt;
        Value stored = curVal ? emitCompoundCombine(*curVal, *rhsVal, bin->op,
                                                   provenNumber(*bin), ilFn)
                              : *rhsVal;
        Value storedBoxed = boxValueIfNeeded(stored, ilFn);

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
        return storedBoxed;
    }
    if (const auto* ident = dynamic_cast<const ast::Ident*>(bin->lhs.get())) {
        const bool compound = bin->op != ast::BinaryOp::Assign;

        // ECMA-262 13.15.2: the target *reference* is evaluated first, and
        // for a compound assignment its current value is read (GetValue)
        // BEFORE the right-hand side is evaluated. So the target is resolved
        // and read here, above `lowerExpr(*bin->rhs)`, exactly as the member
        // and index targets above already do it.
        //
        // Lowering the rhs first is observably wrong whenever the rhs can
        // write the target: `x += f()` where `f` assigns `x` read the
        // post-call value through `env.get` (docs/0007 makes a captured
        // binding memory, so the read is a real instruction that moved with
        // the code), and `x += (x = 3)` folded the inner assignment's value
        // into both operands even for an SSA-backed binding.
        auto it = activeVarMap_.find(ident->name);
        const bool isLocal = it != activeVarMap_.end();
        // An index, not a reference: lowering the rhs can lower a closure,
        // which saves and restores `varBindings_` wholesale (docs/0007), so
        // any reference taken now would be into a replaced vector. The index
        // survives because the restore is of a snapshot taken after this
        // lookup.
        const size_t bindingIdx = isLocal ? it->second : 0;
        uint32_t depth = 0;
        uint32_t index = 0;
        if (!isLocal) {
            // Assignment to a variable captured from an enclosing scope: the
            // closure writes through the environment, so the declaring scope
            // sees it.
            if (currentEnvValue_ == il::kNoValue ||
                !findEnclosingEnvVar(ident->name, depth, index)) {
                diags_.error(ident->span, "undefined variable: " + ident->name);
                return std::nullopt;
            }
        }

        std::optional<Value> curVal;
        if (compound) {
            curVal = isLocal ? readBinding(varBindings_[bindingIdx], ilFn)
                             : emitEnvGet(depth, index, ilFn);
        }

        auto rhsVal = lowerExpr(*bin->rhs, ilFn);
        if (!rhsVal) return std::nullopt;

        // The combine decides numeric-vs-dynamic, and `+=` is numeric only
        // where inference proved it (docs/0010 decision 3). Before that proof
        // existed this path unboxed both sides to f64 unconditionally, so
        // `s += "a"` on a local read a string pointer as a double.
        Value stored = compound ? emitCompoundCombine(*curVal, *rhsVal, bin->op,
                                                      provenNumber(*bin), ilFn)
                                : *rhsVal;
        if (isLocal) {
            writeBinding(varBindings_[bindingIdx], stored, ilFn);
        } else {
            emitEnvSet(depth, index, stored, ilFn);
        }
        return stored;
    }
    // An array or object LITERAL on the left is not a bad target, it is a
    // destructuring assignment — the one form of destructuring the parser
    // cannot name, because both sides parse as ordinary expressions and only
    // the assignment reveals which one it was.
    if (dynamic_cast<const ast::ArrayLit*>(bin->lhs.get()) ||
        dynamic_cast<const ast::ObjectLit*>(bin->lhs.get())) {
        diags_.error(bin->span, "unsupported construct: destructuring assignment");
        return std::nullopt;
    }
    diags_.error(bin->span, "invalid assignment target");
    return std::nullopt;
}

// Where `this` comes from in the function being lowered. An ordinary
// function receives it as the synthetic leading parameter; an arrow has no
// receiver of its own and reads the enclosing function's out of the
// environment chain, under the name no source binding can spell
// (docs/0012 decision 3).
std::optional<Lowerer::Value> Lowerer::lowerThisValue(Span span, il::Function& ilFn) {
    if (currentFunctionIsArrow_) {
        uint32_t depth = 0;
        uint32_t index = 0;
        if (currentEnvValue_ != il::kNoValue && findEnclosingEnvVar("this", depth, index)) {
            return emitEnvGet(depth, index, ilFn);
        }
        diags_.error(span, "`this` outside a function is unsupported");
        return std::nullopt;
    }
    if (currentThisValue_ == il::kNoValue) {
        // usesThis() decided the parameter, so reaching here means `this` at
        // module top level, where its value is a module system question
        // bronze has not answered yet (docs/0008).
        diags_.error(span, "`this` outside a function is unsupported");
        return std::nullopt;
    }
    return Value{currentThisValue_, il::Type::Dynamic};
}

}  // namespace bronze::lower
