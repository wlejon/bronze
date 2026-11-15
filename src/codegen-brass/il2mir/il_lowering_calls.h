#pragma once

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>
#include <unordered_map>

namespace il2mir {

class IlLowering;

bool is_call_il_op(BronzeOp op);

// How many words the function's argv block needs (IlLowering::stage_argv):
// the widest call that stages its arguments — a method call, and any call
// past the fixed-arity helpers.
uint32_t argv_block_words(const IlLowering* lowering, const BronzeFunction& fn_ast);

bool lower_call_instruction(
    IlLowering* lowering,
    const BronzeInstruction& inst_ast,
    Builder& b,
    Function* fn,
    std::unordered_map<uint32_t, Value*>& val_map,
    Value*& res_val
);

} // namespace il2mir
