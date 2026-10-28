#pragma once

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <functional>
#include <unordered_map>

namespace il2mir {

class IlLowering;

bool is_property_il_op(BronzeOp op);

// Whether a PropGet / PropSet's layout claim (il.h `staticSlot`) is lowered
// through its static-slot guard here (il_static_slot.cpp). A claim the module
// has no guard table for is reported as an error and answers false.
bool static_get_lowerable(IlLowering* lowering, const BronzeInstruction& inst_ast);
bool static_set_lowerable(IlLowering* lowering, const BronzeInstruction& inst_ast);

bool lower_property_instruction(
    IlLowering* lowering,
    const BronzeInstruction& inst_ast,
    Builder& b,
    Function* fn,
    std::unordered_map<uint32_t, Value*>& val_map,
    Value*& res_val
);

} // namespace il2mir
