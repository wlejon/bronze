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

// `iter.close`: skips the helper call for runtime-owned iterator kinds and
// already-exhausted records (bronze_abi_tls.h, BRONZE_ABI_ITER_KIND_OWNED_LIMIT_BITS).
void lower_iter_close_inline(Builder& b, Value* iter, int64_t suppress_imm);

}  // namespace il2mir
