#include "opt/speculate/number_slots.h"

#include <algorithm>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace bronze::opt::speculate {

namespace {

// A name's key list, each key as spelled (`!k`, `&k`, `k`), or false for a
// name with no braces or with a brace inside them.
bool splitKeys(const std::string& name, size_t& open, std::vector<std::string>& keys) {
    open = name.find('{');
    if (open == std::string::npos || name.size() < open + 2 || name.back() != '}') return false;
    const std::string_view body = std::string_view(name).substr(open + 1, name.size() - open - 2);
    if (body.find('{') != std::string_view::npos || body.find('}') != std::string_view::npos) return false;
    if (body.empty() || body == "dict") return true;
    for (size_t at = 0; at <= body.size();) {
        const size_t comma = std::min(body.find(',', at), body.size());
        keys.emplace_back(body.substr(at, comma - at));
        at = comma + 1;
    }
    return true;
}

// A key spelled with no mark: a writable data property, the only kind a
// double slot is made for.
bool plainKey(const std::string& spelled) {
    return !spelled.empty() && spelled.front() != '!' && spelled.front() != '&' && spelled.front() != '%' &&
           spelled != "@@symbol";
}

// The keys some `o.k = v` of the module stores a value it can see is no
// Number into: `null`, `undefined`, a boolean, a fresh object or array. The
// harvest cannot see such a store unless it was the last one before the heap
// walk (three.js's render list sets `renderItem.id = null` on the items a
// frame left unused, and reuses them the next frame), and an object that
// meets one moves to the boxed sibling of its shape for good: a speculated
// site of the `%k` name then meets two shapes, and leaves at every object of
// the other one.
std::unordered_set<uint32_t> keysStoredNonNumbers(const il::Module& module) {
    std::unordered_set<uint32_t> out;
    std::vector<const il::Instruction*> defs;
    for (const il::Function& fn : module.functions) {
        defs.clear();
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (inst.result == il::kNoValue) continue;
                if (inst.result >= defs.size()) defs.resize(inst.result + 1, nullptr);
                defs[inst.result] = &inst;
            }
        }
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (inst.op != il::Op::PropSet || inst.operands.size() < 2) continue;
                const il::ValueId v = inst.operands[1];
                const il::Instruction* def = v < defs.size() ? defs[v] : nullptr;
                if (def == nullptr) continue;
                const bool nonNumber = def->op == il::Op::ConstNull || def->op == il::Op::ConstUndefined ||
                                       def->op == il::Op::ObjectLiteral || def->op == il::Op::ArrayLiteral ||
                                       (def->op == il::Op::Box && def->boxType == il::Type::Bool);
                if (nonNumber) out.insert(inst.keyIndex);
            }
        }
    }
    return out;
}

}  // namespace

std::set<std::string> numberSlotKeys(const spec::Harvest& harvest, const il::Module& module) {
    const std::unordered_set<std::string> named(module.keyConstants.begin(), module.keyConstants.end());
    std::unordered_set<std::string> storedOther;
    for (uint32_t k : keysStoredNonNumbers(module)) {
        if (k < module.keyConstants.size()) storedOther.insert(module.keyConstants[k]);
    }
    std::set<std::string> keys;
    for (const auto& [name, positions] : harvest.doubles) {
        size_t open = 0;
        std::vector<std::string> list;
        if (!splitKeys(name, open, list)) continue;
        for (uint32_t p : positions) {
            if (p >= list.size() || !plainKey(list[p])) continue;
            if (harvest.mixed.count(list[p]) != 0 || named.count(list[p]) == 0) continue;
            if (storedOther.count(list[p]) != 0) continue;
            keys.insert(list[p]);
        }
    }
    return keys;
}

spec::Harvest spellNumberSlots(const spec::Harvest& harvest, const std::set<std::string>& keys) {
    spec::Harvest out = harvest;
    if (keys.empty()) return out;
    auto spell = [&](std::string& name) {
        const auto row = harvest.doubles.find(name);
        if (row == harvest.doubles.end()) return;
        size_t open = 0;
        std::vector<std::string> list;
        if (!splitKeys(name, open, list) || list.empty()) return;
        bool changed = false;
        for (uint32_t p : row->second) {
            if (p >= list.size() || !plainKey(list[p]) || keys.count(list[p]) == 0) continue;
            list[p] = "%" + list[p];
            changed = true;
        }
        if (!changed) return;
        std::string spelled = name.substr(0, open + 1);
        for (size_t i = 0; i < list.size(); ++i) {
            if (i != 0) spelled += ",";
            spelled += list[i];
        }
        name = spelled + "}";
    };
    for (auto& [index, site] : out.ics) {
        for (spec::HarvestWay& way : site.ways) spell(way.receiver);
    }
    for (auto& [index, name] : out.last) spell(name);
    return out;
}

}  // namespace bronze::opt::speculate
