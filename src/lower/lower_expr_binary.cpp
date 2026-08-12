// The binary operator families: arithmetic, relational comparison,
// equality (strict and loose), the bitwise and shift operators,
// exponentiation, `in`, `instanceof`, and the comma operator.
//
// Split out of lower_expr.cpp along the grammar's own seam — that file keeps
// the dispatcher, the literals, identifier resolution, the unary operators
// and assignment. What all of these have in common, and what the operand
// helpers at the top exist for, is that each first decides what its operands
// ARE (a double, an int32, a boxed value) and only then which instruction it
// is.

#include <optional>
#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

// ECMA-262 ToInt32, as one instruction. Emitted in front of every operand of
// every bitwise operator — including one that is already an int32, where the
// backend recognizes it and emits nothing, which is what keeps a chain like
// `(a | 0) & 3` from converting twice.
Lowerer::Value Lowerer::emitToInt32(Value val, il::Function& ilFn) {
    if (val.type == il::Type::I32) return val;
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::ToInt32;
    inst.type = il::Type::I32;
    inst.result = res;
    inst.operands = {val.id};
    emitInst(ilFn, inst);
    return Value{res, il::Type::I32};
}

// One bitwise or shift operator: ToInt32 both operands, apply, and read the
// int32 result back as the JS number it denotes.
//
// The result is F64 and not I32 on purpose. `types` has no int32 element (keeps
// the lattice flat), so an I32-typed value reaching a loop header or a call
// would meet a block parameter inference typed `number` and fail to coerce —
// the two runs of the oracle suite would disagree about a program's types. The
// int32 is an intermediate of the operator and never escapes it.
Lowerer::Value Lowerer::emitBitwise(il::Op op, Value lhs, Value rhs, il::Function& ilFn) {
    Value l = emitToInt32(lhs, ilFn);
    Value r = emitToInt32(rhs, ilFn);
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = op;
    inst.type = il::Type::F64;
    inst.result = res;
    inst.operands = {l.id, r.id};
    emitInst(ilFn, inst);
    return Value{res, il::Type::F64};
}

// The IL op behind a bitwise or shift AST operator, or nullopt for anything
// else. One table, so the operator set of `emitBitwise`, of the compound
// assignments and of `lowerBinary` cannot drift apart.
std::optional<il::Op> Lowerer::bitwiseOpFor(ast::BinaryOp op) {
    switch (op) {
        case ast::BinaryOp::BitAnd: return il::Op::BitAnd;
        case ast::BinaryOp::BitOr: return il::Op::BitOr;
        case ast::BinaryOp::BitXor: return il::Op::BitXor;
        case ast::BinaryOp::Shl: return il::Op::Shl;
        case ast::BinaryOp::Shr: return il::Op::Shr;
        case ast::BinaryOp::UShr: return il::Op::UShr;
        default: return std::nullopt;
    }
}

// `a ** b`: ToNumber on both operands, then the one exponentiation
// algorithm the runtime owns (rt_operator.cpp), which `Math.pow` shares.
Lowerer::Value Lowerer::emitPow(Value lhs, Value rhs, il::Function& ilFn) {
    Value l = unboxValueIfNeeded(lhs, il::Type::F64, ilFn);
    Value r = unboxValueIfNeeded(rhs, il::Type::F64, ilFn);
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::Pow;
    inst.type = il::Type::F64;
    inst.result = res;
    inst.operands = {l.id, r.id};
    emitInst(ilFn, inst);
    return Value{res, il::Type::F64};
}

// `a == b` / `a === b` and their negations.
//
// When both operands have the SAME proven primitive type, the two operators
// coincide: step 1 of the abstract equality algorithm is strict equality, and
// strict equality on two doubles or two booleans is one machine compare. Any
// other pairing — including one operand of each type, which used to be three
// separate named errors here — boxes both sides and asks the runtime, which
// is the only place the full algorithm exists.
std::optional<Lowerer::Value> Lowerer::lowerEquality(ast::BinaryOp op, Value lhs, Value rhs,
                                                     il::Function& ilFn) {
    const bool negate = op == ast::BinaryOp::Ne || op == ast::BinaryOp::StrictNe;
    const bool loose = op == ast::BinaryOp::Eq || op == ast::BinaryOp::Ne;

    const bool sameUnboxed = lhs.type == rhs.type &&
                             (lhs.type == il::Type::F64 || lhs.type == il::Type::Bool ||
                              lhs.type == il::Type::I32);

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.type = il::Type::Bool;
    inst.result = res;
    if (sameUnboxed) {
        inst.op = negate ? il::Op::CmpNe : il::Op::CmpEq;
        inst.operands = {lhs.id, rhs.id};
        emitInst(ilFn, inst);
        return Value{res, il::Type::Bool};
    }

    Value lb = boxValueIfNeeded(lhs, ilFn);
    Value rb = boxValueIfNeeded(rhs, ilFn);
    inst.op = loose ? il::Op::LooseEq : il::Op::StrictEq;
    inst.operands = {lb.id, rb.id};
    emitInst(ilFn, inst);
    Value eqRes{res, il::Type::Bool};
    if (!negate) return eqRes;
    return emitLogicalNot(eqRes, ilFn);
}

// `!b` for a value already known to be a bool: compare it against false.
// There is no `not` op — the IL has no unary boolean instruction, and this
// is the shape every negation in lowering already had.
Lowerer::Value Lowerer::emitLogicalNot(Value boolVal, il::Function& ilFn) {
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
    notInst.operands = {boolVal.id, falseVal};
    emitInst(ilFn, notInst);
    return Value{notRes, il::Type::Bool};
}

std::optional<Lowerer::Value> Lowerer::lowerBinary(const ast::Binary* bin, il::Function& ilFn) {
    if (ast::isAssignOp(bin->op)) {
        return lowerAssignment(bin, ilFn);
    }

    if (bin->op == ast::BinaryOp::LogicalAnd || bin->op == ast::BinaryOp::LogicalOr) {
        return lowerLogical(bin, ilFn);
    }

    if (bin->op == ast::BinaryOp::NullishCoalescing) {
        return lowerNullish(bin, ilFn);
    }

    // The comma operator: the left operand is evaluated only for its effects
    // and its value is dropped. No instruction of its own — dropping a value
    // in SSA is not emitting a use of it.
    if (bin->op == ast::BinaryOp::Comma) {
        if (!lowerExpr(*bin->lhs, ilFn)) return std::nullopt;
        return lowerExpr(*bin->rhs, ilFn);
    }

    auto lhsOpt = lowerExpr(*bin->lhs, ilFn);
    if (!lhsOpt) return std::nullopt;
    auto rhsOpt = lowerExpr(*bin->rhs, ilFn);
    if (!rhsOpt) return std::nullopt;

    Value lhs = *lhsOpt;
    Value rhs = *rhsOpt;

    if (const auto bitOp = bitwiseOpFor(bin->op)) {
        return emitBitwise(*bitOp, lhs, rhs, ilFn);
    }
    if (bin->op == ast::BinaryOp::Exp) {
        return emitPow(lhs, rhs, ilFn);
    }
    if (bin->op == ast::BinaryOp::In || bin->op == ast::BinaryOp::InstanceOf) {
        // Both are predicates over runtime identity — a shape chain for
        // `in`, a prototype chain for `instanceof` — so both operands are
        // boxed and the runtime answers.
        Value lb = boxValueIfNeeded(lhs, ilFn);
        Value rb = boxValueIfNeeded(rhs, ilFn);
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = bin->op == ast::BinaryOp::In ? il::Op::In : il::Op::InstanceOf;
        inst.type = il::Type::Bool;
        inst.result = res;
        inst.operands = {lb.id, rb.id};
        emitInst(ilFn, inst);
        return Value{res, il::Type::Bool};
    }
    if (bin->op == ast::BinaryOp::Eq || bin->op == ast::BinaryOp::StrictEq ||
        bin->op == ast::BinaryOp::Ne || bin->op == ast::BinaryOp::StrictNe) {
        return lowerEquality(bin->op, lhs, rhs, ilFn);
    }

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

}  // namespace bronze::lower
