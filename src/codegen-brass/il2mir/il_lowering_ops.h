#pragma once

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <functional>
#include <unordered_map>

namespace il2mir {

class IlLowering;

bool is_ops_il_op(BronzeOp op);

// ToBoolean of a dynamic (i64) value as an i32 0/1; inline for every operand
// whose answer is its bits unless `helper_only`.
Value* lower_dynamic_truthy(Builder& b, Value* v, bool helper_only);

bool lower_ops_instruction(
    IlLowering* lowering,
    const BronzeInstruction& inst_ast,
    Builder& b,
    Function* fn,
    std::unordered_map<uint32_t, Value*>& val_map,
    Value*& res_val
);

} // namespace il2mir
