#include "lower/typed_loop_uses.h"

namespace bronze::lower {

std::vector<std::vector<Use>> computeUses(const il::Function& fn) {
    std::vector<std::vector<Use>> uses(fn.valueCount);
    for (const auto& blk : fn.blocks) {
        for (uint32_t i = 0; i < blk.instructions.size(); ++i) {
            const il::Instruction& inst = blk.instructions[i];
            for (uint32_t k = 0; k < inst.operands.size(); ++k) {
                const il::ValueId v = inst.operands[k];
                if (v < uses.size()) uses[v].push_back({blk.id, i, Use::Operand, k});
            }
            for (il::ValueId v : inst.target.args) {
                if (v < uses.size()) uses[v].push_back({blk.id, i, Use::ThenArg, 0});
            }
            for (il::ValueId v : inst.elseTarget.args) {
                if (v < uses.size()) uses[v].push_back({blk.id, i, Use::ElseArg, 0});
            }
        }
    }
    return uses;
}

il::ValueId numberSource(const il::Function& fn, const std::vector<DefSite>& defs, il::ValueId v, bool allowI32) {
    if (v >= defs.size() || defs[v].block == il::kNoBlock || defs[v].index == UINT32_MAX) return il::kNoValue;
    const il::Instruction& def = fn.blocks[defs[v].block].instructions[defs[v].index];
    if (def.op != il::Op::Box || def.operands.size() != 1) return il::kNoValue;
    if (def.boxType == il::Type::F64 || (allowI32 && def.boxType == il::Type::I32)) return def.operands[0];
    return il::kNoValue;
}

bool isCoercingF64Use(const il::Instruction& inst, uint32_t slot) {
    switch (inst.op) {
        case il::Op::Add:
        case il::Op::Sub:
        case il::Op::Mul:
        case il::Op::Div:
        case il::Op::Mod:
        case il::Op::Pow:
        case il::Op::Neg:
            return inst.type == il::Type::F64;
        case il::Op::CmpLt:
        case il::Op::CmpLe:
        case il::Op::CmpGt:
        case il::Op::CmpGe:
        case il::Op::CmpEq:
        case il::Op::CmpNe:
        case il::Op::MathUnary:
            return true;
        case il::Op::ToInt32:
            return inst.boxType == il::Type::F64;
        case il::Op::ElemSetTyped:
            return slot == 2;
        default:
            return false;
    }
}

}  // namespace bronze::lower
