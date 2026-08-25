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
    // The operand's type, which is what tells the backend whether this
    // conversion can throw: 7.1.6 step 1 is ToNumber, and for a BOXED operand
    // that reaches a user `valueOf` and the Symbol TypeError.
    inst.boxType = val.type;
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
    // A boxed operand keeps its box and the operator becomes the dynamic form.
    // ToInt32 is the NUMBER half of 13.15.3 only: on two BigInts these
    // operators are defined over the whole values, and converting first would
    // silently truncate `(1n << 64n) & mask` to 32 bits.
    if (lhs.type == il::Type::Dynamic || rhs.type == il::Type::Dynamic) {
        Value lb = boxValueIfNeeded(lhs, ilFn);
        Value rb = boxValueIfNeeded(rhs, ilFn);
        il::ValueId dres = ilFn.valueCount++;
        il::Instruction dinst;
        dinst.op = op;
        dinst.type = il::Type::Dynamic;
        dinst.result = dres;
        dinst.operands = {lb.id, rb.id};
        emitInst(ilFn, dinst);
        return Value{dres, il::Type::Dynamic};
    }
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
    if (lhs.type == il::Type::Dynamic || rhs.type == il::Type::Dynamic) {
        Value lb = boxValueIfNeeded(lhs, ilFn);
        Value rb = boxValueIfNeeded(rhs, ilFn);
        il::ValueId dres = ilFn.valueCount++;
        il::Instruction dinst;
        dinst.op = il::Op::Pow;
        dinst.type = il::Type::Dynamic;
        dinst.result = dres;
        dinst.operands = {lb.id, rb.id};
        emitInst(ilFn, dinst);
        return Value{dres, il::Type::Dynamic};
    }
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

// `a < b`, `a > b`, `a <= b`, `a >= b` — ECMA-262 13.10.
//
// The operand types choose between two algorithms, and the choice is a
// correctness question rather than a speed one. 13.10.1 IsLessThan step 3 asks
// whether BOTH operands are Strings after ToPrimitive and, if they are,
// compares them by code unit and converts nothing; ToNumeric is step 4, the
// else-branch. A `dynamic` operand may be a string, so only a proof that
// neither side is boxed licenses the machine compare — sending a boxed operand
// down the F64 path is what made `"a" < "b"` a comparison of two NaNs.
//
// The other half of this is what is NOT here any more: `a <= b` used to be
// emitted as `!(a > b)`. That identity needs a total order, and NaN does not
// give one. 13.10 answers false when IsLessThan produces undefined — 13.10.1
// step 4.c, either operand NaN — while the negation maps that same undefined to
// true. cmp.le and cmp.ge are ordered compares and answer false for NaN
// directly, which is what `<` and `>` were already doing right.
std::optional<Lowerer::Value> Lowerer::lowerRelational(ast::BinaryOp op, Value lhs, Value rhs,
                                                       il::Function& ilFn) {
    // Stated as the types that ARE numeric rather than as "not dynamic": a
    // native string type would satisfy the negative form and take the f64 path,
    // which is the defect in its next disguise.
    auto isNumeric = [](il::Type t) {
        return t == il::Type::F64 || t == il::Type::I32 || t == il::Type::Bool;
    };
    const bool provenNumeric = isNumeric(lhs.type) && isNumeric(rhs.type);

    il::Op ilOp;
    switch (op) {
        case ast::BinaryOp::Less: ilOp = provenNumeric ? il::Op::CmpLt : il::Op::RelLt; break;
        case ast::BinaryOp::Greater: ilOp = provenNumeric ? il::Op::CmpGt : il::Op::RelGt; break;
        case ast::BinaryOp::LessEqual: ilOp = provenNumeric ? il::Op::CmpLe : il::Op::RelLe; break;
        default: ilOp = provenNumeric ? il::Op::CmpGe : il::Op::RelGe; break;
    }

    Value l = provenNumeric ? unboxValueIfNeeded(lhs, il::Type::F64, ilFn)
                            : boxValueIfNeeded(lhs, ilFn);
    Value r = provenNumeric ? unboxValueIfNeeded(rhs, il::Type::F64, ilFn)
                            : boxValueIfNeeded(rhs, ilFn);

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = ilOp;
    inst.type = il::Type::Bool;
    inst.result = res;
    inst.operands = {l.id, r.id};
    emitInst(ilFn, inst);
    return Value{res, il::Type::Bool};
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

    // `#x in o` (13.10.1) is a BRAND check, not a property lookup: it asks
    // whether the object carries the private element, which nothing on the
    // property path can answer. The parser admits a PrivateIdentifier in this
    // one operand of this one operator, so recognizing it here is total.
    if (bin->op == ast::BinaryOp::In) {
        if (const auto* nameIdent = dynamic_cast<const ast::Ident*>(bin->lhs.get());
            nameIdent && !nameIdent->name.empty() && nameIdent->name[0] == '#') {
            return lowerPrivateIn(*bin->lhs, *bin->rhs, ilFn);
        }
    }

    // An operand this operator coerces on every branch may be a proven
    // typed-array element read, in which case it lowers straight to an
    // unboxed f64 (lower_typed_elem.cpp has the argument). Same evaluation,
    // same order — only the instruction differs.
    const bool lhsCoerces = binaryCoercesOperand(bin->op, *bin->rhs);
    const bool rhsCoerces = binaryCoercesOperand(bin->op, *bin->lhs);
    auto lhsOpt = lhsCoerces ? lowerCoercingOperand(*bin->lhs, ilFn) : lowerExpr(*bin->lhs, ilFn);
    if (!lhsOpt) return std::nullopt;
    auto rhsOpt = rhsCoerces ? lowerCoercingOperand(*bin->rhs, ilFn) : lowerExpr(*bin->rhs, ilFn);
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
    if (bin->op == ast::BinaryOp::Less || bin->op == ast::BinaryOp::Greater ||
        bin->op == ast::BinaryOp::LessEqual || bin->op == ast::BinaryOp::GreaterEqual) {
        return lowerRelational(bin->op, lhs, rhs, ilFn);
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
        // The other four take the same fork `+` has always taken, and for the
        // reason `+` did not used to need company: a boxed operand may be a
        // BigInt, and 13.15.3 over a BigInt is a DIFFERENT algorithm — exact,
        // and a TypeError against a Number — not ToNumber followed by an fsub.
        // Unboxing first is what made `1n - 1n` produce 0 through two NaNs.
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
        default:
            diags_.error(bin->span, "unsupported binary operator: " + std::string(ast::binaryOpName(bin->op)));
            return std::nullopt;
    }

    // A boxed operand keeps its box and the instruction becomes the dynamic
    // form, whose helper owns ToNumeric, the BigInt algorithm and the mixing
    // TypeError. The relational operators do NOT belong here — they left
    // through lowerRelational above, because ToNumeric is only one of their
    // two branches.
    // With no BigInt anywhere in the program, ToNumeric IS ToNumber and the
    // BigInt arm of 13.15.3 is dead code: `a * b` is exactly
    // `ToNumber(a) * ToNumber(b)`, which is this operand pair unboxed and a
    // native f64 multiply. Taking that road rather than the boxed one is worth
    // far more than the two conversions it saves, because an f64 result is not
    // a GC root: `planFrame` roots Dynamic values and only those, and a
    // rooted value is stored to its slot after its defining instruction and
    // reloaded before every use. A chain like `a*b + c*d` over dynamic operands
    // keeps every intermediate in a register under this arm and spills all of
    // them under the other.
    //
    // `unboxValueIfNeeded` emits the two conversions in source order, which is
    // the order 13.15.3 evaluates them in — a `valueOf` on the left runs before
    // one on the right, as it must.
    const bool numericArith = !numericArithDisabled_ && resType == il::Type::F64;
    if ((lhs.type == il::Type::Dynamic || rhs.type == il::Type::Dynamic) &&
        !numericArith) {
        lhs = boxValueIfNeeded(lhs, ilFn);
        rhs = boxValueIfNeeded(rhs, ilFn);
        resType = il::Type::Dynamic;
    } else if (resType == il::Type::F64) {
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
