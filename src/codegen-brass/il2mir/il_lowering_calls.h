#pragma once

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>
#include <functional>
#include <unordered_map>

namespace il2mir {

class IlLowering;

bool is_call_il_op(BronzeOp op);

bool lower_call_instruction(
    IlLowering* lowering,
    const BronzeInstruction& inst_ast,
    Builder& b,
    Function* fn,
    std::unordered_map<uint32_t, Value*>& val_map,
    Value*& res_val
);

} // namespace il2mir
