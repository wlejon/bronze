#pragma once

// The module's keyed-access stubs (il_elem_stub.cpp): bronze_elem_get's and
// bronze_elem_set's operands and answers, with the computed-access caches'
// plain-object hits inline.

#include "il_ast.h"
#include <brass/mir/builder.hpp>

namespace il2mir {

class IlLowering;

inline constexpr const char* kElemGetStub = "__bronze_elem_get_keyed";
inline constexpr const char* kElemSetStub = "__bronze_elem_set_keyed";
// What a constant-index access (`c[0]`, lowered as a dense Array's element,
// il_property_lowering.cpp) calls when its receiver is no dense Array: a word
// view's element answered here, the keyed stub for everything else.
inline constexpr const char* kIndexGetStub = "__bronze_index_get";
inline constexpr const char* kIndexSetStub = "__bronze_index_set";

// Builds the stubs (once, before any body) unless the build counts every
// computed access in the helpers, and tells the property lowering which.
void emit_elem_stubs(IlLowering* lowering, Module& mod);

class PropertyLoweringHelper;

// ToInt32 of a double with no call (il_elem_lowering.cpp): the low 32 bits an
// integer view stores, for every Number (NaN and the infinities are 0). The
// builder is left in the block where the answer is defined.
Value* narrow_f64_to_i32_callfree(Builder& b, Value* v);

// An element key as the Value the helpers and stubs take: an i32 is an int32
// Number, an f64 its boxed bits, anything else already a Value.
Value* boxed_elem_key(Builder& b, Value* index);

// The get stub's own-slot hit inlined at a site: the slot's value branches to
// `merge` (its one i64 parameter), everything else to `miss`, which calls the
// stub.
void build_inline_elem_get_hit(PropertyLoweringHelper& pl, Builder& b, Value* obj, Value* key, BasicBlock* merge,
                               BasicBlock* miss);

}  // namespace il2mir
