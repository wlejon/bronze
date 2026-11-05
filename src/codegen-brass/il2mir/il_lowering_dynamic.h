#pragma once

// The dynamic (NaN-boxed) forms of `+ - * /`, the bitwise ops, `===`, `==`,
// the relational compares, unary `-` and ToBoolean (il_lowering_dynamic.cpp):
// an inline arm for operands whose answer is in their bits, the runtime
// helper for the rest.
//
// `hot` is a type-feedback hint (il.h `numberHint`, docs/type-feedback.md):
// every operand the profiled run saw at the site was a Number. The Number arm
// then falls through and every other arm, the helper call included, is a cold
// block, which brass's block layout sinks to the end of the function. A wrong
// hint moves blocks; it never changes an answer.

#include "il_ast.h"
#include <brass/mir/builder.hpp>

namespace il2mir {

class IlLowering;

Value* lower_dynamic_arith(IlLowering* lowering, Builder& b, BronzeOp op, Value* op0, Value* op1,
                           const char* helper, bool hot = false);
Value* lower_dynamic_bitwise(Builder& b, BronzeOp op, Value* op0, Value* op1, const char* helper,
                             bool hot = false);
Value* lower_dynamic_strict_eq(Builder& b, Value* op0, Value* op1, bool helper_only, bool hot = false);
Value* lower_dynamic_relational(Builder& b, BronzeOp op, Value* op0, Value* op1, const char* helper,
                                bool helper_only, bool hot = false);
// ToBoolean of a dynamic (i64) value as an i32 0/1; inline for every operand
// whose answer is its bits unless `helper_only`. A type-feedback profile's
// view of the site: `number_hint` (il.h `numberHint`, only Numbers seen), or
// `kinds` (il.h `truthHint`, the kTruth* kinds seen when every value was one
// of them). The kinds seen are tested first and the rest is laid out cold.
Value* lower_dynamic_truthy(Builder& b, Value* v, bool helper_only, bool number_hint = false, uint8_t kinds = 0);
// The hinted forms of two ops that are a helper call without a hint: `==`
// with two Numbers as the ordered double compare, unary `-` of a Number as
// the double's negation, each with the helper in a cold block.
Value* lower_hinted_loose_eq(Builder& b, Value* op0, Value* op1);
Value* lower_hinted_neg(Builder& b, Value* op0);

// Whether the inline ToInt32 applies to the Value (or f64) bits `bits`: a
// Number of magnitude below 2^63. NaN, the infinities and every non-Number
// fail it.
Value* int32_convertible(Builder& b, Value* bits);
// ToInt32 of the double `f`, where int32_convertible holds.
Value* inline_to_int32(Builder& b, Value* f);
// Whether `v` is an undefined, null or Boolean constant (or a boxed Boolean):
// bits no Number, String or BigInt has, so `===` against it is a compare.
bool is_bits_identity_constant(const Value* v);
// Whether two String headers (payload pointers) with different bits are proven
// to hold different text, as an i32.
Value* build_strings_differ(Builder& b, Value* s0, Value* s1);

} // namespace il2mir
