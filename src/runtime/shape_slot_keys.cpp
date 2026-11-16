// The slot-key table of a shape (shape.h `slot_keys`, bronze_abi_layout.h
// BRONZE_ABI_SHAPE_SLOTKEYS_OFFSET): for every slot, which string key the
// shape's own properties keep there, whether it is a data property (boxed or
// double), writable, and a double slot. Generated code's `shape.has_own` test reads it
// (il2mir/il_lowering_spec.cpp), so a site whose receivers come in many shapes
// that all keep the key at one slot tests the slot's entry instead of each
// shape's identity.
//
// Built on first ask, per shape, in the thread's arena (shapes are immortal
// and so is their table), from the shape's own transition chain: every node
// owns exactly one property and one slot (two for an accessor pair), and a
// node is never changed once made, so the table never goes stale. The absent
// cache in front of it (bronze_abi_layout.h BRONZE_ABI_SLOTKEY_LACKS_ENTRIES)
// is the one part that changes: accessor_cell.cpp bronze_shape_lacks fills
// it, and an entry speaks for the epoch it names only.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/heap.h"
#include "runtime/profile.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"

namespace bronze::runtime {

namespace {

const uint64_t* buildSlotKeys(const Shape* shape) {
    std::vector<const Shape*> nodes;
    uint32_t count = 0;
    // A dictionary keeps its properties in its table, not in a chain: an
    // empty table, whose every test fails.
    if (!shape->isDictionary()) {
        for (const Shape* node = shape; node != nullptr && node->parent != nullptr; node = node->parent) {
            nodes.push_back(node);
            count = std::max(count, node->slot_index + (node->accessor ? 2u : 1u));
        }
    }
    // The absent cache (bronze_abi_layout.h BRONZE_ABI_SLOTKEY_LACKS_ENTRIES)
    // sits before word 0, empty until a `shape.lacks` miss fills an entry.
    constexpr size_t kLacksWords = BRONZE_ABI_SLOTKEY_LACKS_BYTES / sizeof(uint64_t);
    auto* block = static_cast<uint64_t*>(rtArena().allocate(sizeof(uint64_t) * (kLacksWords + 1 + count)));
    std::fill(block, block + kLacksWords, uint64_t{0});
    uint64_t* table = block + kLacksWords;
    table[0] = count;
    std::fill(table + 1, table + 1 + count, uint64_t{0});
    for (const Shape* node : nodes) {
        const StringHeader* name = node->key.string();
        if (name == nullptr) continue;  // a symbol: no key index names it
        uint32_t index = 0;
        if (!rtKeyIndexOf(rtUtf8Chars(name), index)) continue;  // no module names it
        uint64_t entry = (uint64_t{index} + 1) << BRONZE_ABI_SLOTKEY_KEY_SHIFT;
        if (!node->accessor && node->repr != SlotRepr::Object) {
            entry |= BRONZE_ABI_SLOTKEY_DATA;
            if (node->writable) entry |= BRONZE_ABI_SLOTKEY_WRITABLE;
            if (node->repr == SlotRepr::Double) entry |= BRONZE_ABI_SLOTKEY_NUMBER;
        }
        table[1 + node->slot_index] = entry;
    }
    return table;
}

}  // namespace

extern "C" uint64_t bronze_shape_slot_keys(uint64_t shapeBits) {
    recordHelperCall("bronze_shape_slot_keys");
    auto* shape = reinterpret_cast<Shape*>(shapeBits);
    if (shape->slot_keys == nullptr) shape->slot_keys = buildSlotKeys(shape);
    return reinterpret_cast<uint64_t>(shape->slot_keys);
}

}  // namespace bronze::runtime
