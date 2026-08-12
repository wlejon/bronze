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

// `-`, `*`, `/` and `%` are ToNumber on both operands, so the result is a
// number whatever came in (NaN is a number). `+` is concatenation as soon as
// either side is a string, because ToPrimitive on a string operand wins
// however the other side prints.
Type arithResult(ast::BinaryOp op, Type l, Type r);

// The bitwise, shift and exponentiation operators are ToInt32/ToNumber on
// both operands, so the result is a number whatever came in — including a
// string operand, which `+` is the only operator to treat differently.
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
