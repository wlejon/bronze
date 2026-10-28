#pragma once

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>
#include <functional>
#include <unordered_map>

namespace il2mir {

class IlLowering;

bool is_call_il_op(BronzeOp op);

// Whether a method call is lowered as a speculated builtin (a `Math` member
// whose target and argument a CallTarget site tests), which stages no argv.
bool method_call_is_speculated_builtin(const IlLowering* lowering, const BronzeInstruction& inst);

// How many words the function's argv block needs (IlLowering::stage_argv):
// the widest call that stages its arguments — a dynamic method call, any call
// past the fixed-arity helpers, and, where calls enter callees directly
// (il_lowering_direct.h), every call, `new` and closure call.
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
