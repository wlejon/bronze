#pragma once

// The iteration record's value and close, inline (il_iter_lowering.cpp).

#include "il_ast.h"
#include <brass/mir/builder.hpp>

namespace il2mir {

// `iter.value`: the record's `current` word.
Value* lower_iter_value_inline(Builder& b, Value* iter);

// `iter.close`: skips the helper call for runtime-owned iterator kinds and
// already-exhausted records (bronze_abi_tls.h, BRONZE_ABI_ITER_KIND_OWNED_LIMIT_BITS).
void lower_iter_close_inline(Builder& b, Value* iter, int64_t suppress_imm);

}  // namespace il2mir
