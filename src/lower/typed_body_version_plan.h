#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "il/il.h"
#include "il/il_pin.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

struct TypedBodyGuardSplit {
    uint32_t index = 0;
    std::vector<il::ValueId> guarded;
    std::vector<il::ValueId> params;
    std::vector<il::ValueId> paramIds;
};

// Entry bounds/non-NaN witness for a receiver whose in-body `ElemGet`s all
// address `baseVal + offset` (`0 <= offset <= maxOffset`) before any
// collection/detachment point. `baseVal == kNoValue` denotes constant indices.
struct TypedBodyReadProbe {
    size_t recvIdx = 0;
    il::ValueId baseVal = il::kNoValue;
    int32_t maxOffset = 0;
};

struct TypedBodyPlan {
    il::BlockId entry = il::kNoBlock;
    std::vector<il::BlockId> body;
    std::vector<uint8_t> in;
    std::vector<il::ValueId> receivers;
    std::vector<int32_t> receiverKinds;
    std::vector<std::vector<int32_t>> versionKinds;
    std::vector<uint8_t> provenInBoundsRead;
    std::vector<TypedBodyReadProbe> readProbes;
    std::vector<il::ValueId> guardedPreBody;
    std::unordered_set<il::ValueId> numericAdd;
    std::vector<std::vector<TypedBodyGuardSplit>> splitsOf;
    size_t totalSplits = 0;
    std::vector<il::Type> types;
    std::vector<DefSite> defs;
    std::unordered_map<il::ValueId, std::vector<il::ValueId>> rawUnboxes;
};

inline bool isBodyElemAccess(const il::Instruction& inst) {
    return (inst.op == il::Op::ElemGet || inst.op == il::Op::ElemSet) && inst.operands.size() >= 2 &&
           !il::isDefineWrite(inst);
}

inline bool isBodyKnownNonNumberDef(const il::Instruction& def) {
    switch (def.op) {
        case il::Op::ConstNull:
        case il::Op::ConstUndefined:
        case il::Op::ConstBool:
        case il::Op::ConstBigInt:
        case il::Op::CreateObject:
        case il::Op::CreateArray:
        case il::Op::CreateFunction:
        case il::Op::EnvCreate:
        case il::Op::TemplateObject:
        case il::Op::TemplateCached:
        case il::Op::ConcatBegin:
        case il::Op::ConcatAppend:
        case il::Op::ConcatEnd:
        case il::Op::ToStr:
            return true;
        case il::Op::Box:
            return def.boxType != il::Type::F64 && def.boxType != il::Type::I32;
        default:
            return false;
    }
}

inline bool isBodyFloatArithOp(il::Op op) {
    return op == il::Op::Sub || op == il::Op::Mul || op == il::Op::Div || op == il::Op::Mod ||
           op == il::Op::Neg;
}

inline bool isBodyBitwiseBinOp(il::Op op) {
    return op == il::Op::BitAnd || op == il::Op::BitOr || op == il::Op::BitXor ||
           op == il::Op::Shl || op == il::Op::Shr || op == il::Op::UShr;
}

inline bool isBodyRelOp(il::Op op) {
    return op == il::Op::RelLt || op == il::Op::RelGt || op == il::Op::RelLe ||
           op == il::Op::RelGe;
}

inline il::Op promotedBodyCmpOp(il::Op op) {
    switch (op) {
        case il::Op::StrictEq: return il::Op::CmpEq;
        case il::Op::RelLt: return il::Op::CmpLt;
        case il::Op::RelGt: return il::Op::CmpGt;
        case il::Op::RelLe: return il::Op::CmpLe;
        case il::Op::RelGe: return il::Op::CmpGe;
        default: return op;
    }
}

inline il::ValueId bodyDominatingRawUnboxAt(
    const Cfg& cfg, const std::vector<DefSite>& defs,
    const std::unordered_map<il::ValueId, std::vector<il::ValueId>>& rawUnboxes, il::ValueId v,
    il::BlockId b, uint32_t idx) {
    const auto it = rawUnboxes.find(v);
    if (it == rawUnboxes.end()) return il::kNoValue;
    for (il::ValueId u : it->second) {
        const DefSite& du = defs[u];
        if ((du.block == b && du.index < idx) || (du.block != b && cfg.dominates(du.block, b))) {
            return u;
        }
    }
    return il::kNoValue;
}

inline bool isBodyPromotableDynOp(const il::Function& fn, const TypedBodyPlan& plan,
                                  const il::Instruction& inst) {
    if (inst.type != il::Type::Dynamic || inst.result == il::kNoValue) return false;
    auto nonNum = [&](il::ValueId v) {
        if (v >= plan.defs.size() || plan.defs[v].block == il::kNoBlock ||
            plan.defs[v].index == UINT32_MAX) {
            return false;
        }
        return isBodyKnownNonNumberDef(fn.blocks[plan.defs[v].block].instructions[plan.defs[v].index]);
    };
    if (inst.op == il::Op::Neg) return inst.operands.size() == 1 && !nonNum(inst.operands[0]);
    if (inst.operands.size() != 2) return false;
    if (nonNum(inst.operands[0]) || nonNum(inst.operands[1])) return false;
    if (inst.op == il::Op::Add) return plan.numericAdd.count(inst.result) != 0;
    return isBodyFloatArithOp(inst.op) || isBodyBitwiseBinOp(inst.op);
}

bool emitTypedBodyVersion(il::Function& fn, const Cfg& cfg, TypedBodyPlan& plan,
                          std::vector<il::BlockId>& coldOut);

}  // namespace bronze::lower
