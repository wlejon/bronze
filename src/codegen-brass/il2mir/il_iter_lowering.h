#pragma once

// The iteration record's step and value, inline (il_iter_lowering.cpp).

#include "il_ast.h"
#include <brass/mir/builder.hpp>

namespace il2mir {

// `iter.step` with a Bool result: an ARRAY walk's next present element read
// inline, bronze_iter_step for everything else. Returns the i32 0/1.
Value* lower_iter_step_inline(Builder& b, Value* iter);

// `iter.value`: the record's `current` word.
Value* lower_iter_value_inline(Builder& b, Value* iter);

}  // namespace il2mir
