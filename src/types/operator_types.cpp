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

Type arithResult(ast::BinaryOp op, Type l, Type r) {
    if (l.is(TypeKind::Never) || r.is(TypeKind::Never)) return Type::never();
    if (op != ast::BinaryOp::Add) return Type::number();
    if (l.is(TypeKind::String) || r.is(TypeKind::String)) return Type::string();
    if (isNumericPrimitive(l) && isNumericPrimitive(r)) return Type::number();
    return Type::dynamic();
}

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
        case ast::UnaryOp::Negate:
        case ast::UnaryOp::Posate:
        // `~` is ToInt32 then a complement, so a number however it came.
        case ast::UnaryOp::BitNot: return withBottom(operand, Type::number());
        // The one operator whose result type is the same for every operand
        // there is: one of six strings.
        case ast::UnaryOp::TypeOf: return withBottom(operand, Type::string());
        // `void x` yields undefined, always; x is evaluated for effect.
        case ast::UnaryOp::Void: return withBottom(operand, Type::undefined());
        // ToNumber, so the binding holds a number afterwards whatever it held
        // before. NOT `withBottom`: the update reads and writes a binding, and
        // a binding no value has reached is not a reason to call the result ⊥.
        case ast::UnaryOp::PreInc:
        case ast::UnaryOp::PreDec:
        case ast::UnaryOp::PostInc:
        case ast::UnaryOp::PostDec: return Type::number();
    }
    return Type::dynamic();
}

Type binaryResult(ast::BinaryOp op, Type l, Type r) {
    if (isAlwaysNumericOp(op)) return withBottom(l, r, Type::number());
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
    return isAlwaysNumericOp(plainOp) ? withBottom(current, rhs, Type::number())
                                      : arithResult(plainOp, current, rhs);
}

}  // namespace bronze::types
