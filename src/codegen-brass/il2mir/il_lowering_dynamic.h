#pragma once

// The dynamic (NaN-boxed) forms of `+ - * /`, the bitwise ops, `===`, `==`,
// the relational compares, unary `-` and ToBoolean (il_lowering_dynamic.cpp):
// an inline arm for operands whose answer is in their bits, the runtime
// helper for the rest.

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <unordered_map>

namespace il2mir {

class IlLowering;
class PropertyLoweringHelper;

Value* lower_dynamic_arith(IlLowering* lowering, Builder& b, BronzeOp op, Value* op0, Value* op1,
                           const char* helper);
Value* lower_dynamic_bitwise(Builder& b, BronzeOp op, Value* op0, Value* op1, const char* helper);
// `arms`: the instruction's slow arm (il_property.h slow_branch), the
// helper's call here and in lower_dynamic_truthy.
Value* lower_dynamic_strict_eq(Builder& b, Value* op0, Value* op1, PropertyLoweringHelper* arms = nullptr);
Value* lower_dynamic_relational(Builder& b, BronzeOp op, Value* op0, Value* op1, const char* helper);
// ToBoolean of a dynamic (i64) value as an i32 0/1, inline for every operand
// whose answer is its bits.
Value* lower_dynamic_truthy(Builder& b, Value* v, PropertyLoweringHelper* arms = nullptr);

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
