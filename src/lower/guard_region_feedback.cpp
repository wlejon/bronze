// GUARDED REGIONS, the profile's half (guard_region.h, "PROFILE-GUIDED
// REGIONS"): which ops a recorded run says seed a region, which operands it
// says poison one, and the compares a fast copy promotes.

#include <algorithm>
#include <map>

#include "il/feedback_site.h"
#include "lower/guard_region.h"

namespace bronze::lower {

bool isPromotableCompare(const il::Instruction& inst, const std::vector<il::Type>& types) {
    switch (inst.op) {
        case il::Op::RelLt:
        case il::Op::RelGt:
        case il::Op::RelLe:
        case il::Op::RelGe:
        case il::Op::LooseEq:
            break;
        case il::Op::StrictEq:
            // A dispatch test's false edge is correct for any operands and its
            // true edge only for one String; it is not `===` and is not
            // promoted to it.
            if (inst.immI32 & il::Instruction::kStrictEqIdentityDispatch) return false;
            break;
        default:
            return false;
    }
    if (inst.type != il::Type::Bool || inst.result == il::kNoValue || inst.operands.size() != 2) return false;
    for (il::ValueId id : inst.operands) {
        if (id >= types.size() || types[id] != il::Type::Dynamic) return false;
    }
    return true;
}

il::Op promotedCompareOp(il::Op op) {
    switch (op) {
        case il::Op::RelLt: return il::Op::CmpLt;
        case il::Op::RelGt: return il::Op::CmpGt;
        case il::Op::RelLe: return il::Op::CmpLe;
        case il::Op::RelGe: return il::Op::CmpGe;
        default: return il::Op::CmpEq;  // strict.eq, loose.eq
    }
}

ProfiledOp profiledOp(const RegionFeedback& feedback, const il::Instruction& inst,
                      const std::vector<il::Type>& types) {
    ProfiledOp out;
    if (!feedback.operand || !il::observesOperandTags(inst)) return out;
    const bool promotable =
        isPromotableArith(inst, types) || isCheckedUnboxOf(inst, types) || isPromotableCompare(inst, types);
    bool all = true;
    bool any = false;
    uint64_t executions = 0;
    for (uint32_t j = 0; j < inst.operands.size(); ++j) {
        const il::ValueId v = inst.operands[j];
        if (v >= types.size() || types[v] != il::Type::Dynamic) continue;
        const OperandFeedback f = feedback.operand(inst, j);
        if (f.verdict == OperandFeedback::Other) {
            out.poisoned.push_back(v);
            all = false;
        } else if (f.verdict == OperandFeedback::Unknown) {
            all = false;
        } else {
            any = true;
            executions = std::max(executions, f.executions);
        }
    }
    out.seed = promotable && all && any;
    out.executions = out.seed ? executions : 0;
    return out;
}

uint32_t markConfirmedSlowCopiesCold(il::Function& fn, const RegionFeedback& feedback) {
    if (!feedback.operand || !feedback.entries) return 0;
    const uint64_t entries = feedback.entries(fn);
    if (entries < kFeedbackHotExecutions) return 0;
    const std::vector<il::Type> types = functionValueTypes(fn);
    // Per region: whether its slow copy saw a non-Number, and its busiest site.
    struct Verdict {
        bool other = false;
        uint64_t busiest = 0;
    };
    std::map<uint32_t, Verdict> regions;
    for (const il::Block& block : fn.blocks) {
        if (block.copyClass != il::CopyClass::Slow || block.cold) continue;
        Verdict& v = regions[block.copyRegion];
        for (const il::Instruction& inst : block.instructions) {
            if (!il::observesOperandTags(inst)) continue;
            for (uint32_t j = 0; j < inst.operands.size(); ++j) {
                const il::ValueId id = inst.operands[j];
                if (id >= types.size() || types[id] != il::Type::Dynamic) continue;
                const OperandFeedback f = feedback.operand(inst, j);
                if (f.verdict == OperandFeedback::Other) v.other = true;
                v.busiest = std::max(v.busiest, f.executions);
            }
        }
    }
    auto coldRegion = [&](const Verdict& v) { return !v.other && v.busiest * 16 <= entries; };
    auto confirmed = [&](const il::Block& block) {
        if (block.copyClass != il::CopyClass::Slow) return false;
        const auto it = regions.find(block.copyRegion);
        return it != regions.end() && coldRegion(it->second);
    };
    uint32_t marked = 0;
    for (const auto& [region, v] : regions) marked += coldRegion(v) ? 1 : 0;
    if (marked == 0) return 0;
    // The trampolines: a fast block whose one exit is a jump into a confirmed
    // slow copy (a fast copy leaves for its slow copy nowhere else).
    for (il::Block& block : fn.blocks) {
        if (block.copyClass != il::CopyClass::Fast || block.instructions.empty()) continue;
        const il::Instruction& term = block.instructions.back();
        if (term.op != il::Op::Jump || term.target.block >= fn.blocks.size()) continue;
        if (confirmed(fn.blocks[term.target.block])) block.cold = true;
    }
    for (il::Block& block : fn.blocks) {
        if (confirmed(block)) block.cold = true;
    }
    return marked;
}

}  // namespace bronze::lower
