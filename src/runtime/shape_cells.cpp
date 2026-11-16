// The shape cells (shape_cells.h).

#include "runtime/shape_cells.h"

#include "abi/bronze_abi.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/type_feedback.h"

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bronze::runtime {

namespace {

// A cell waiting for its shape.
struct PendingCell {
    uint64_t* cell;
    std::string key;
    uint64_t epoch;
};

struct Registry {
    std::vector<PendingCell> pending;
    // Pending cells by the last key of their name (`` for none): what a new
    // node must have added for its name to be one of theirs. Rebuilt after
    // any removal.
    std::unordered_map<std::string, std::vector<size_t>> byLastKey;
};

thread_local Registry t_cells;

// A name parsed: `<prototype>[#N]{k1,...,kn}`. A key holding `,` or `{` does
// not parse back; such a cell is found only by a new shape's full name.
struct ParsedName {
    std::string proto;
    uint32_t inlineSlots = Shape::kDefaultInlineSlots;
    std::vector<std::string> keys;
    bool dict = false;
};

bool parseName(std::string_view name, ParsedName& out) {
    const size_t brace = name.find('{');
    if (brace == std::string_view::npos || name.empty() || name.back() != '}') return false;
    std::string_view head = name.substr(0, brace);
    const std::string_view body = name.substr(brace + 1, name.size() - brace - 2);
    if (const size_t hash = head.rfind('#'); hash != std::string_view::npos) {
        uint32_t n = 0;
        for (char c : head.substr(hash + 1)) {
            if (c < '0' || c > '9') return false;
            n = n * 10 + static_cast<uint32_t>(c - '0');
        }
        out.inlineSlots = n;
        head = head.substr(0, hash);
    }
    out.proto = std::string(head);
    if (body == "dict") {
        out.dict = true;
        return true;
    }
    size_t at = 0;
    while (at < body.size()) {
        const size_t comma = std::min(body.find(',', at), body.size());
        out.keys.emplace_back(body.substr(at, comma - at));
        at = comma + 1;
    }
    return true;
}

std::string lastKeyOf(std::string_view name) {
    ParsedName p;
    if (!parseName(name, p) || p.keys.empty()) return std::string();
    return p.keys.back();
}

void reindex(Registry& r) {
    r.byLastKey.clear();
    for (size_t i = 0; i < r.pending.size(); ++i) r.byLastKey[lastKeyOf(r.pending[i].key)].push_back(i);
}

// Whether a slot load and store may touch every property of an object at
// `shape` directly (shape_cells.h).
bool eligible(const Shape* shape) {
    if (shape == nullptr || shape->isDictionary() || shape->used_as_prototype || shape->uncacheable ||
        shape->has_symbol_keys || shape->double_slots != 0 || shape->object_slots != 0) {
        return false;
    }
    for (const Shape* n = shape; n != nullptr && n->parent != nullptr; n = n->parent) {
        if (n->accessor || !n->writable || n->repr != SlotRepr::Boxed || !n->key.isString()) return false;
    }
    return true;
}

bool keyIs(PropertyKey key, const std::string& name) {
    const StringHeader* s = key.string();
    return s != nullptr && rtUtf8Chars(s) == name;
}

// The eligible shape under `root` whose keys are `keys[at..]` added in order
// below `from`, or null.
const Shape* descend(const Shape* from, const std::vector<std::string>& keys, size_t at) {
    if (at == keys.size()) return eligible(from) ? from : nullptr;
    for (const ShapeTransition& t : from->transitions) {
        if (t.accessor || !t.writable || t.repr != SlotRepr::Boxed || !keyIs(t.key, keys[at])) continue;
        if (const Shape* found = descend(t.next_shape, keys, at + 1)) return found;
    }
    return nullptr;
}

void fill(uint64_t* cell, const Shape* shape) {
    *cell = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(shape));
}

}  // namespace

void rtShapeCellsNoteShape(const Shape* shape) {
    Registry& r = t_cells;
    if (r.pending.empty() || shape == nullptr || !eligible(shape)) return;
    // A root is noted when it is made, which for a class's instances is
    // before its prototype has the `constructor` its name is read from; its
    // first child asks again for it.
    if (shape->parent != nullptr && shape->root != nullptr && shape->root != shape) {
        rtShapeCellsNoteShape(shape->root);
        if (r.pending.empty()) return;
    }
    std::string last;
    if (shape->parent != nullptr) {
        const StringHeader* s = shape->key.string();
        if (s == nullptr) return;
        last = rtUtf8Chars(s);
    }
    const auto it = r.byLastKey.find(last);
    if (it == r.byLastKey.end()) return;
    std::unordered_map<const Shape*, std::string> protoNames;
    const std::string name = typeFeedbackShapeName(shape, protoNames);
    bool filled = false;
    for (size_t i : it->second) {
        PendingCell& p = r.pending[i];
        if (p.cell == nullptr || p.key != name) continue;
        fill(p.cell, shape);
        p.cell = nullptr;
        filled = true;
    }
    if (!filled) return;
    std::erase_if(r.pending, [](const PendingCell& p) { return p.cell == nullptr; });
    reindex(r);
}

void rtShapeCellsDropWithin(const void* begin, size_t size) {
    Registry& r = t_cells;
    if (r.pending.empty() || begin == nullptr || size == 0) return;
    const auto lo = reinterpret_cast<uintptr_t>(begin);
    const uintptr_t hi = lo + size;
    const size_t before = r.pending.size();
    std::erase_if(r.pending, [lo, hi](const PendingCell& p) {
        const auto a = reinterpret_cast<uintptr_t>(p.cell);
        return a >= lo && a < hi;
    });
    if (r.pending.size() != before) reindex(r);
}

void rtShapeCellsDropEpoch(uint64_t epoch) {
    Registry& r = t_cells;
    if (r.pending.empty()) return;
    const size_t before = r.pending.size();
    std::erase_if(r.pending, [epoch](const PendingCell& p) { return p.epoch == epoch; });
    if (r.pending.size() != before) reindex(r);
}

}  // namespace bronze::runtime

using namespace bronze;
using namespace bronze::runtime;

// `keys`: `count` names, each a u32 byte length and its bytes
// (il2mir::kShapeCellKeysSymbol); `cells`: `count` words, this thread's.
extern "C" void bronze_register_shape_cells(uint64_t* cells, const uint8_t* keys, uint32_t count) {
    if (cells == nullptr || keys == nullptr || count == 0) return;
    std::vector<std::string> names;
    names.reserve(count);
    const uint8_t* at = keys;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t len = 0;
        std::memcpy(&len, at, sizeof len);
        at += sizeof len;
        names.emplace_back(reinterpret_cast<const char*>(at), len);
        at += len;
    }

    // The shapes that exist now: down each root's transition tree along the
    // name's keys, from the roots whose prototype has the name's prototype
    // part.
    std::vector<ParsedName> parsed(count);
    std::vector<bool> open(count, false);
    bool any = false;
    for (uint32_t i = 0; i < count; ++i) {
        cells[i] = 0;
        open[i] = parseName(names[i], parsed[i]) && !parsed[i].dict;
        any = any || open[i];
    }
    if (any) {
        std::unordered_map<const Shape*, std::string> protoNames;
        rtForEachRootShape([&](Shape* root) {
            if (root->uncacheable || root->used_as_prototype) return;
            std::string rootName;
            bool named = false;
            for (uint32_t i = 0; i < count; ++i) {
                if (!open[i] || cells[i] != 0 || root->inline_slots != parsed[i].inlineSlots) continue;
                if (!named) {
                    // The root's own name, `<prototype>[#N]{}`, less its braces.
                    rootName = typeFeedbackShapeName(root, protoNames);
                    rootName.resize(rootName.rfind('{'));
                    if (const size_t hash = rootName.rfind('#'); hash != std::string::npos &&
                                                                 root->inline_slots != Shape::kDefaultInlineSlots) {
                        rootName.resize(hash);
                    }
                    named = true;
                }
                if (rootName != parsed[i].proto) continue;
                if (const Shape* found = descend(root, parsed[i].keys, 0)) fill(&cells[i], found);
            }
        });
    }

    // The rest wait for their shape.
    Registry& r = t_cells;
    const uint64_t epoch = rtCurrentModuleEpoch();
    for (uint32_t i = 0; i < count; ++i) {
        if (cells[i] == 0) r.pending.push_back({&cells[i], std::move(names[i]), epoch});
    }
    reindex(r);
}
