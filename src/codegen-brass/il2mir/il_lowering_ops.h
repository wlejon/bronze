#pragma once

#include "il_ast.h"
#include <brass/mir/builder.hpp>
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
// The f64 an i64 Value was boxed from by box_f64_bits, or null. With `here`,
// only a box made in that block: what a lowering may use in the box's place
// (one from an earlier block may be from before a resume point, whose state
// does not carry it; il_lowering_resume.cpp).
Value* boxed_f64_source(Value* v, const BasicBlock* here = nullptr);
bool lower_ops_instruction(
    IlLowering* lowering,
    const BronzeInstruction& inst_ast,
    Builder& b,
    Function* fn,
    std::unordered_map<uint32_t, Value*>& val_map,
    Value*& res_val
);

} // namespace il2mir
