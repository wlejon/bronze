#pragma once

// The allocation-free lookup of a Map, Set, WeakMap or WeakSet table
// (map.cpp MapHeader::findFast / findFastIdentity), inline so the collection
// natives' own fast paths (builtin_map.cpp, builtin_weak_map.cpp
// bronze_collection_op) make it without a call. Answers only from an index
// that ensureIndex would keep as it is; `false` means the caller must take
// the rooted path, which may rebuild it.

#include <bit>
#include <cstdint>

#include "runtime/heap.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze {

// The hash SameValueZero implies (map.cpp): a string by its characters, a
// Number with -0 and NaN normalized, a BigInt by its limbs, anything else by
// its bits.
uint32_t mapHashKey(Value v) noexcept;

namespace map_probe {

// `indexEpoch` when no key the index hashes by address can move: every such
// key is old, and an old object never moves, so no collection invalidates the
// index. Inserting a movable key sets the real epoch.
constexpr double kStableIndex = -2.0;
constexpr uint64_t kStableIndexBits = std::bit_cast<uint64_t>(kStableIndex);
static_assert(kStableIndexBits == 0xC000000000000000ULL);

// splitmix64's finalizer: cheap, and it moves every input bit into the low
// ones, which is what a power-of-two bucket mask reads.
inline uint32_t mix64(uint64_t x) noexcept {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return static_cast<uint32_t>(x);
}

// The index's bucket table and mask when ensureIndex would keep it as it is
// (index present, epoch unmoved, anchor unmoved), else nullptr. The index is
// an object-tagged block or undefined. Epochs and heap addresses are below
// 2^63, so converting them as signed gives the doubles the index stored.
inline const uint32_t* usableBuckets(const Heap& heap, const MapHeader* map, uint32_t& mask) noexcept {
    const Value index = map->index;
    if (!index.isObject()) return nullptr;
    if (map->indexEpoch.rawBits() != kStableIndexBits &&
        map->indexEpoch.asNumber() != static_cast<double>(static_cast<int64_t>(heap.relocation_epoch()))) {
        return nullptr;
    }
    if (map->indexAnchor.asNumber() !=
        static_cast<double>(static_cast<int64_t>(reinterpret_cast<uintptr_t>(map)))) {
        return nullptr;
    }
    const auto* hdr = index.asObject<HeapObjectHeader>();
    const auto count = static_cast<uint32_t>((hdr->size - sizeof(HeapObjectHeader)) / sizeof(uint32_t));
    if (count == 0) return nullptr;
    mask = count - 1;
    return hdr->payload<uint32_t>();
}

// The probe. A key whose bits are equal is the same key (a tombstone, a hole,
// is never a key), which is the whole of SameValueZero for an object, a
// symbol, a boolean, null and undefined; a string, a Number or a BigInt that
// differs in bits is compared by value. kIdentity: the caller knows the key
// is an object or a symbol. An empty table answers from its index like any
// other (a nonzero bucket implies an entry block, which is the only time the
// entries are read).
template <bool kIdentity>
inline bool probeEntry(const Heap& heap, MapHeader* map, Value key, uint32_t& slot,
                       const Value*& outEntries) noexcept {
    uint32_t mask = 0;
    const uint32_t* buckets = usableBuckets(heap, map, mask);
    if (buckets == nullptr) return false;
    const uint64_t want = key.rawBits();
    uint32_t h;
    bool byValue = !kIdentity;
    if (kIdentity) {
        h = mix64(want);
    } else if (key.isString()) {
        const auto* str = key.asString<StringHeader>();
        const uint32_t f = str->flags;
        h = mix64((f & StringHeader::kHasHashFlag) != 0 ? (f & 0xFFFFFFFCU) : str->hash());
    } else if (key.isNumber() || key.isBigInt()) {
        h = mapHashKey(key);
    } else {
        h = mix64(want);
        byValue = false;
    }
    uint32_t b = h & mask;
    if (buckets[b] == 0) {
        slot = UINT32_MAX;
        return true;
    }
    const Value* entries = map->entries.asObject<HeapObjectHeader>()->payload<Value>();
    outEntries = entries;
    for (; buckets[b] != 0; b = (b + 1) & mask) {
        const uint32_t s = buckets[b] - 1;
        const Value k = entries[s * 2];
        if (k.rawBits() == want || (byValue && !k.isHole() && sameValueZero(k, key))) {
            slot = s;
            return true;
        }
    }
    slot = UINT32_MAX;
    return true;
}

template <bool kIdentity>
inline bool probe(const Heap& heap, MapHeader* map, Value key, uint32_t& slot) noexcept {
    const Value* entries = nullptr;
    return probeEntry<kIdentity>(heap, map, key, slot, entries);
}

template <bool kIdentity>
inline bool probeValue(const Heap& heap, MapHeader* map, Value key, bool& found, Value& outVal) noexcept {
    uint32_t slot = UINT32_MAX;
    const Value* entries = nullptr;
    if (!probeEntry<kIdentity>(heap, map, key, slot, entries)) return false;
    found = slot != UINT32_MAX;
    outVal = found ? entries[slot * 2 + 1] : Value::fromUndefined();
    return true;
}

// Whether the plain object `obj`, which a guard found at a collection's
// instance shape, carries [[MapData]] under `brand` (MapHeader::hasBrand,
// with the internal-slot count read off the size the shape implies).
inline bool branded(const ObjectHeader* obj, Value brand) noexcept {
    if (obj->header.flags != HeapKind::Plain) return false;
    const size_t base = obj->shape->bare_header >> 32;
    if (obj->header.size != base + CollectionSlot::kCount * sizeof(Value)) return false;
    return reinterpret_cast<const MapHeader*>(obj)->brand.rawBits() == brand.rawBits();
}

}  // namespace map_probe
}  // namespace bronze
