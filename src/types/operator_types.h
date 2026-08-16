#pragma once

#include "ast/ast.h"
#include "types/type.h"

namespace bronze::types {

// What each operator's result type is, as a pure function of its operands'.
//
// Split from the flow analysis because it is the one part of inference with a
// second, independent implementation: lowering decides the same question when
// it picks an IL op and a block-parameter type, and the two answers have to
// agree exactly or the inferred run and the `--no-infer` run disagree about a
// join. A rule that lives in one named place can be read against lowering; a
// rule spread through a walker cannot.
//
// Nothing here evaluates anything: the operands' effects are the walker's
// business, and every function below is total on the lattice.

// ⊥ in, ⊥ out: an operand no value has reached yet cannot produce one. This is
// what lets a recursive function read its own not-yet-known return type without
// the estimate jumping straight to `Dynamic`.
Type withBottom(Type operand, Type result);
Type withBottom(Type a, Type b, Type result);

// Whether ToNumber on this type is the whole story — the test `+` makes
// observable, because it is the one arithmetic operator that concatenates.
bool isNumericPrimitive(Type t);

// Can a value of this type still be a BigInt when an operator gets it? Only
// `dynamic` and an object (whose `valueOf` may return one) can; a String
// cannot, because 7.1.3 ToNumeric routes it through ToNumber and never through
// StringToBigInt.
bool isNeverBigInt(Type t);

// The result of an operator whose two branches are Number::op and BigInt::op:
// a number when neither operand can be a BigInt, and DYNAMIC otherwise, since
// the lattice has no BigInt element and calling one a number would licence an
// f64 fast path to read a heap pointer.
Type numericOrDynamic(Type l, Type r);

// `-`, `*`, `/` and `%` are ToNumERIC on both operands, so the result is a
// number when neither operand can be a BigInt and dynamic when one can. `+` is
// concatenation as soon as either side is a string, because ToPrimitive on a
// string operand wins however the other side prints.
Type arithResult(ast::BinaryOp op, Type l, Type r);

// The bitwise, shift and exponentiation operators, which are one family
// because ToNumeric is their ONLY coercion — there is no string branch, the
// way `+` has one. Their RESULT type is not shared: on two BigInts every one
// of them produces a BigInt.
bool isAlwaysNumericOp(ast::BinaryOp op);

// The prefix and postfix operators. The update forms also SHARPEN the binding
// they write, which is the walker's job and not stated here; this is only the
// value the expression produces.
Type unaryResult(ast::UnaryOp op, Type operand);

// The binary operators that are neither assignments nor short-circuiting —
// the ones whose result is a function of both operand types and nothing else.
Type binaryResult(ast::BinaryOp op, Type l, Type r);

// `x op= y`, whose result is `x op y` and whose written value is that result.
Type compoundResult(ast::BinaryOp plainOp, Type current, Type rhs);

}  // namespace bronze::types
