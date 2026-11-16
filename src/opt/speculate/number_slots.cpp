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

}  // namespace

std::set<std::string> numberSlotKeys(const spec::Harvest& harvest, const il::Module& module) {
    const std::unordered_set<std::string> named(module.keyConstants.begin(), module.keyConstants.end());
    std::set<std::string> keys;
    for (const auto& [name, positions] : harvest.doubles) {
        size_t open = 0;
        std::vector<std::string> list;
        if (!splitKeys(name, open, list)) continue;
        for (uint32_t p : positions) {
            if (p >= list.size() || !plainKey(list[p])) continue;
            if (harvest.mixed.count(list[p]) != 0 || named.count(list[p]) == 0) continue;
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
