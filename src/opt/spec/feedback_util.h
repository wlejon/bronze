#pragma once

// What the type-feedback planner and its consumer both read of a function
// (feedback_plan.h): which values a feedback site observes, by the rule
// il::enumerateFeedbackSites applies — a Dynamic value not held as raw double
// bits (`numberBits`).

#include <cstdint>
#include <vector>

#include "il/il.h"

namespace bronze::spec {

inline std::vector<uint8_t> observableValues(const il::Function& fn) {
    std::vector<il::Type> types(fn.valueCount, il::Type::Void);
    std::vector<uint8_t> out(fn.valueCount, 0);
    for (size_t p = 0; p < fn.params.size() && p < types.size(); ++p) types[p] = fn.params[p].type;
    std::vector<uint8_t> doubleHeld(fn.valueCount, 0);
    for (const il::Block& block : fn.blocks) {
        for (const il::BlockParam& bp : block.params) {
            if (bp.id < types.size()) types[bp.id] = bp.type;
        }
        for (const il::Instruction& inst : block.instructions) {
            if (inst.result == il::kNoValue || inst.result >= types.size()) continue;
            types[inst.result] = inst.type;
            if (inst.numberBits) doubleHeld[inst.result] = 1;
        }
    }
    for (size_t v = 0; v < out.size(); ++v) out[v] = types[v] == il::Type::Dynamic && doubleHeld[v] == 0;
    return out;
}

}  // namespace bronze::spec
