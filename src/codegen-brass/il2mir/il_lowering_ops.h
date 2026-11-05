#pragma once

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <functional>
#include <unordered_map>

namespace il2mir {

class IlLowering;

bool is_ops_il_op(BronzeOp op);

// The f64 `f` as a Value's bits, every NaN the canonical one (a NaN of any
// other sign or payload would read as a tagged value). The NaN test is
// `f != f`: one ucomisd and a cmovp, where testing the bits takes a mask, a
// compare and two 64-bit constants, and the form brass's number_bits reads
// as an ordered Number.
Value* box_f64_bits(Builder& b, Value* f);
// The f64 an i64 Value was boxed from by box_f64_bits, or null.
Value* boxed_f64_source(Value* v);

// `box.str k` for the global key id `key_id` (i32): bronze_box_str_key's
// answer, read inline from the thread's key-header table (il_box_str_key.cpp).
Value* lower_box_str_key(Builder& b, Value* key_id);

// `x === lit` where `lit` is a `box.str k` (il_box_str_key.cpp): equal bits
// answer true by one compare, and only a String with other bits goes on.
Value* lower_strict_eq_string_literal(Builder& b, Value* x, Value* lit);

bool lower_ops_instruction(
    IlLowering* lowering,
    const BronzeInstruction& inst_ast,
    Builder& b,
    Function* fn,
    std::unordered_map<uint32_t, Value*>& val_map,
    Value*& res_val
);

} // namespace il2mir
