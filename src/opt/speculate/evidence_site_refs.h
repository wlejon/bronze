#pragma once

// Evidence::SiteRefs (evidence.h), shared by the planners of evidence.cpp and
// evidence_call.cpp.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "il/il.h"
#include "opt/speculate/context.h"
#include "opt/speculate/evidence.h"

namespace bronze::opt::speculate {

// The value types and defining instructions of one function.
struct Evidence::SiteRefs {
    std::vector<il::Type> types;
    // Per value, the instruction defining it (null for parameters and block
    // parameters).
    std::vector<const il::Instruction*> defs;
    // Per block parameter, the one value its edges pass (kNoValue otherwise).
    std::vector<ValueId> incoming;

    SiteRefs(const il::Module& /*module*/, const il::Function& fn)
        : types(valueTypes(fn)), defs(fn.valueCount, nullptr) {
        std::unordered_map<BlockId, const il::Block*> byId;
        for (const il::Block& b : fn.blocks) {
            byId[b.id] = &b;
            for (const il::Instruction& inst : b.instructions) {
                if (inst.result != il::kNoValue && inst.result < defs.size()) defs[inst.result] = &inst;
            }
        }
        // A block parameter every edge passes one value (or itself, or a
        // parameter that is in turn that value) is that value: a loop's
        // unchanged `var` (single). Resolved to a fixed point, as trivial
        // phis are.
        incoming.assign(fn.valueCount, il::kNoValue);
        std::unordered_map<ValueId, std::vector<ValueId>> args;
        auto edge = [&](const il::BlockTarget& t) {
            auto it = byId.find(t.block);
            if (it == byId.end()) return;
            const auto& params = it->second->params;
            for (size_t i = 0; i < params.size() && i < t.args.size(); ++i) args[params[i].id].push_back(t.args[i]);
        };
        for (const il::Block& b : fn.blocks) {
            for (const il::Instruction& inst : b.instructions) {
                edge(inst.target);
                edge(inst.elseTarget);
            }
        }
        bool changed = true;
        for (int round = 0; changed && round < 16; ++round) {
            changed = false;
            for (const auto& [param, from] : args) {
                if (param >= incoming.size()) continue;
                ValueId only = il::kNoValue;
                bool many = false;
                for (ValueId a : from) {
                    a = single(a);
                    if (a == param) continue;
                    if (only == il::kNoValue) only = a;
                    else if (only != a) many = true;
                }
                const ValueId now = many ? il::kNoValue : only;
                if (now != incoming[param]) {
                    incoming[param] = now;
                    changed = true;
                }
            }
        }
    }
    const il::Instruction* def(ValueId v) const { return v < defs.size() ? defs[v] : nullptr; }
    // `v`, or the one value it stands for through block parameters.
    ValueId single(ValueId v) const {
        for (int hops = 0; hops < 8 && v < incoming.size() && incoming[v] != il::kNoValue; ++hops) v = incoming[v];
        return v;
    }
    static uint64_t key(uint32_t block, uint32_t inst) { return (uint64_t{block} << 32) | inst; }
    il::Type type(ValueId v) const { return v < types.size() ? types[v] : il::Type::Void; }
};

}  // namespace bronze::opt::speculate
