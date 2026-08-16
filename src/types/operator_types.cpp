#include "types/operator_types.h"

namespace bronze::types {

Type withBottom(Type operand, Type result) {
    return operand.is(TypeKind::Never) ? Type::never() : result;
}

Type withBottom(Type a, Type b, Type result) {
    return (a.is(TypeKind::Never) || b.is(TypeKind::Never)) ? Type::never() : result;
}

bool isNumericPrimitive(Type t) {
    switch (t.kind()) {
        case TypeKind::Number:
        case TypeKind::Bool:
        case TypeKind::Null:
        case TypeKind::Undefined: return true;
        default: return false;
    }
}

// Can a value of this type still be a BIGINT when the operator gets it? What
// decides is 7.1.3 ToNumeric, which has exactly two rows that do not end in a
// Number: a BigInt stays one, and an object may return one from `valueOf`.
// Everything else — including a String, which goes through ToNumber and never
// through StringToBigInt, so `"5" - 1` really is the number 4 — is safe.
//
// It is a NEGATIVE test on purpose: a type element added tomorrow answers
// "might be" and loses a fast path, rather than answering "is a number" and
// putting a heap pointer's bits into a double.
bool isNeverBigInt(Type t) {
    switch (t.kind()) {
        case TypeKind::Number:
        case TypeKind::Bool:
        case TypeKind::Null:
        case TypeKind::Undefined:
        case TypeKind::String: return true;
        default: return false;
    }
}

// The result of an operator that is numeric on both its branches: a Number
// when neither operand can be a BigInt, and otherwise DYNAMIC — because the
// BigInt branch produces a BigInt and the lattice has no element for it.
Type numericOrDynamic(Type l, Type r) {
    if (l.is(TypeKind::Never) || r.is(TypeKind::Never)) return Type::never();
    return isNeverBigInt(l) && isNeverBigInt(r) ? Type::number() : Type::dynamic();
}

Type arithResult(ast::BinaryOp op, Type l, Type r) {
    if (l.is(TypeKind::Never) || r.is(TypeKind::Never)) return Type::never();
    if (op != ast::BinaryOp::Add) return numericOrDynamic(l, r);
    if (l.is(TypeKind::String) || r.is(TypeKind::String)) return Type::string();
    if (isNumericPrimitive(l) && isNumericPrimitive(r)) return Type::number();
    return Type::dynamic();
}

// The bitwise, shift and exponentiation operators. Named for what makes them
// one family — they have no string branch, so ToNumeric is their ONLY
// coercion — rather than for a result type they no longer share: on two
// BigInts every one of them produces a BigInt.
bool isAlwaysNumericOp(ast::BinaryOp op) {
    switch (op) {
        case ast::BinaryOp::BitAnd:
        case ast::BinaryOp::BitOr:
        case ast::BinaryOp::BitXor:
        case ast::BinaryOp::Shl:
        case ast::BinaryOp::Shr:
        case ast::BinaryOp::UShr:
        case ast::BinaryOp::Exp: return true;
        default: return false;
    }
}

Type unaryResult(ast::UnaryOp op, Type operand) {
    switch (op) {
        case ast::UnaryOp::Not: return withBottom(operand, Type::boolean());
        // `+x` is ToNUMBER (13.5.4), which is the one unary numeric operator
        // with no BigInt branch at all: it throws rather than producing one,
        // so its result is a Number whenever it has a result.
        case ast::UnaryOp::Posate: return withBottom(operand, Type::number());
        // `-x` and `~x` are ToNUMERIC, so a BigInt operand gives a BigInt
        // back and the answer is only a Number when the operand cannot be one.
        case ast::UnaryOp::Negate:
        case ast::UnaryOp::BitNot: return numericOrDynamic(operand, operand);
        // The one operator whose result type is the same for every operand
        // there is: one of six strings.
        case ast::UnaryOp::TypeOf: return withBottom(operand, Type::string());
        // `void x` yields undefined, always; x is evaluated for effect.
        case ast::UnaryOp::Void: return withBottom(operand, Type::undefined());
        // `delete` yields a boolean, always — and NOT `withBottom`: its
        // operand is a reference that is never read, so a ⊥ operand type
        // says nothing about whether the delete happens.
        case ast::UnaryOp::Delete: return Type::boolean();
        // ToNumERIC (13.4.2.1 step 2), so `n++` on a BigInt leaves a BigInt
        // behind and the binding's type follows the operand's. NOT
        // `withBottom`: the update reads and writes a binding, and a binding no
        // value has reached is not a reason to call the result ⊥.
        case ast::UnaryOp::PreInc:
        case ast::UnaryOp::PreDec:
        case ast::UnaryOp::PostInc:
        case ast::UnaryOp::PostDec:
            return isNeverBigInt(operand) ? Type::number() : Type::dynamic();
    }
    return Type::dynamic();
}

Type binaryResult(ast::BinaryOp op, Type l, Type r) {
    if (isAlwaysNumericOp(op)) return numericOrDynamic(l, r);
    switch (op) {
        case ast::BinaryOp::Add:
        case ast::BinaryOp::Sub:
        case ast::BinaryOp::Mul:
        case ast::BinaryOp::Div:
        case ast::BinaryOp::Mod: return arithResult(op, l, r);
        case ast::BinaryOp::Less:
        case ast::BinaryOp::Greater:
        case ast::BinaryOp::LessEqual:
        case ast::BinaryOp::GreaterEqual:
        case ast::BinaryOp::Eq:
        case ast::BinaryOp::StrictEq:
        case ast::BinaryOp::Ne:
        case ast::BinaryOp::StrictNe:
        // `in` and `instanceof` are predicates: whatever their operands are,
        // the answer is a boolean.
        case ast::BinaryOp::In:
        case ast::BinaryOp::InstanceOf: return withBottom(l, r, Type::boolean());
        // The comma operator's value IS its right operand, and its left
        // operand contributes only effects.
        case ast::BinaryOp::Comma: return r;
        default: break;
    }
    return Type::dynamic();
}

Type compoundResult(ast::BinaryOp plainOp, Type current, Type rhs) {
    if (plainOp == ast::BinaryOp::LogicalAnd || plainOp == ast::BinaryOp::LogicalOr ||
        plainOp == ast::BinaryOp::NullishCoalescing) {
        return join(current, rhs);
    }
    return isAlwaysNumericOp(plainOp) ? numericOrDynamic(current, rhs)
                                      : arithResult(plainOp, current, rhs);
}

}  // namespace bronze::types
