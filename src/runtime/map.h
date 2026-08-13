#pragma once

#include <cstdint>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

// `Map` and `Set`. The first bronze structure whose KEY is a value rather than
// a property name, so none of the machinery under `o.k` applies: no shape, no
// interned name, no slot index. What a Map has instead is an insertion-ordered
// entry table plus a hash index over it.
//
// The two share one layout, because a Set is a Map whose values are its keys
// (24.2.1.1 defines it that way and every method below reads the same table);
// `header.flags` is what tells them apart and what decides which methods
// `bronze_prop_get` hands out.
//
// Everything the collector must see is a Value in the payload, so the generic
// payload scan forwards the table without this file owning a root source. The
// hash INDEX is the exception and is deliberately RawBytes-tagged: it holds
// entry indices, not references, and the collector must not read it as Values.
struct MapHeader {
    HeapObjectHeader header;
    // Object-tagged block of Values, two per entry slot: key then value, in
    // INSERTION order. A removed entry's key is the Hole singleton, which is
    // internal by construction and therefore cannot collide with a key a
    // program can hold. Erasing from the middle would move every later entry
    // and break the iteration order a live iterator is holding a cursor into,
    // so a delete tombstones instead.
    Value entries;
    // RawBytes block of uint32 buckets, open-addressed with linear probing.
    // 0 means empty; anything else is an entry slot index plus one.
    Value index;
    Value liveCount;   // double: entries a program can see
    Value usedCount;   // double: entry slots handed out, tombstones included
    // double: `Heap::relocation_epoch()` when `index` was last built. An
    // object key hashes by ADDRESS and the collector moves objects, so a
    // relocation invalidates every object key's bucket. Rebuilding lazily on
    // the next lookup is what keeps that from being a correctness bug rather
    // than a cost; see the header comment of Heap::relocation_epoch for why
    // the number counts relocations and not collections.
    Value indexEpoch;
    // double: this map's OWN address when `index` was last built, as an
    // independent witness that nothing has moved. It is here because the
    // epoch, however carefully placed, is a number some future collector
    // could forget to touch — and this one it cannot forget: to relocate any
    // object a collector must trace from the roots, a live map is on that
    // trace, and moving the map changes this. Neither check subsumes the
    // other (an epoch catches a map that happened to land back on its old
    // address; the anchor catches a collector that bypassed the epoch), so
    // the index is valid only when BOTH agree.
    Value indexAnchor;

    static constexpr uint16_t kMapFlags = HeapKind::Map;
    static constexpr uint16_t kSetFlags = HeapKind::Set;
    // A WeakMap and a WeakSet reuse this table wholesale — brand-checked by
    // their own kinds, so nothing that dispatches on a Map's flags ever sees
    // one. The entries are STRONG references for now; builtin_weak_map.cpp's
    // header says why that is observably correct and where true weakness
    // would hang.
    static constexpr uint16_t kWeakMapFlags = HeapKind::WeakMap;
    static constexpr uint16_t kWeakSetFlags = HeapKind::WeakSet;

    static MapHeader* create(Heap& heap, uint16_t flags);

    uint32_t liveSize() const noexcept { return static_cast<uint32_t>(liveCount.asNumber()); }
    uint32_t used() const noexcept { return static_cast<uint32_t>(usedCount.asNumber()); }
    // How many entry slots the current block holds, derived from the block
    // rather than stored: two facts that can disagree are one fact too many.
    uint32_t capacity() const noexcept;

    Value* entryData() noexcept {
        if (!entries.isPointer()) return nullptr;
        return entries.asObject<HeapObjectHeader>()->payload<Value>();
    }
    const Value* entryData() const noexcept {
        if (!entries.isPointer()) return nullptr;
        return entries.asObject<HeapObjectHeader>()->payload<Value>();
    }

    Value keyAt(uint32_t slot) const noexcept { return entryData()[slot * 2]; }
    Value valueAt(uint32_t slot) const noexcept { return entryData()[slot * 2 + 1]; }
    bool liveAt(uint32_t slot) const noexcept { return !entryData()[slot * 2].isHole(); }

    // The entry slot holding `key`, or UINT32_MAX. Rebuilds the index first
    // if a collection has run since it was built — which is why BOTH the map
    // and the key arrive through roots: that rebuild allocates, so a raw key
    // taken by value would be read after a collection had moved it.
    static uint32_t find(Heap& heap, Rooted<Value>& self, Rooted<Value>& key);

    // Insert or update. Updating an EXISTING key keeps its position, which is
    // 24.1.3.9 step 4 and the reason the table is ordered at all.
    static void set(Heap& heap, Rooted<Value>& self, Rooted<Value>& key, Rooted<Value>& val);

    // True when something was removed. Tombstones the slot; the key and value
    // are dropped so the collector stops holding them live.
    static bool remove(Heap& heap, Rooted<Value>& self, Rooted<Value>& key);

    static void clear(Rooted<Value>& self);
};

// ECMA-262 7.2.10 SameValueZero: `===` except that NaN matches NaN. `+0` and
// `-0` match under both, which `==` on the raw bits would get wrong, and two
// distinct string objects with the same characters are one key, which a bit
// compare would get wrong the other way.
bool sameValueZero(Value a, Value b) noexcept;

}  // namespace bronze
