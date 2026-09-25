#pragma once

#include <cstddef>
#include <cstdint>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace bronze {

// The table under `Map` and `Set` — and under a WeakMap, a WeakSet and a
// private-element table, which reuse it. The first bronze structure whose KEY
// is a value rather than a property name, so none of the machinery under
// `o.k` applies: no interned name, no slot index. What the table has instead
// is an insertion-ordered entry list plus a hash index over it.
//
// The object CARRYING the table is an ORDINARY OBJECT WITH INTERNAL SLOTS,
// which is exactly what 24.1.4 says a Map is: [[MapData]] is a slot on an
// object that otherwise has a shape, a [[Prototype]] and own properties like
// any other. So a `MapHeader` is an `ObjectHeader` followed by the inline
// property slots every object has, followed by the table's fields as internal
// slots (`ObjectHeader::createWithInternalSlots`), and `m.foo = 1` lands in
// the property half while `m.set("foo", 1)` lands in the table half, the two
// never meeting. That arrangement is what gives a Map a real `Map.prototype`
// on its chain and lets `m.get` be found by the ordinary property walk.
//
// Which KIND of collection an object is — a Map, a Set, a WeakMap — is its
// BRAND: the first internal slot holds a symbol minted once per kind by the
// kind's own file (builtin_map.cpp, builtin_weak_map.cpp, rt_private.cpp) and
// unreachable from any program, so `Object.create(Map.prototype)` can never
// pass for a Map. `hasBrand` is the one test every dispatch asks.
//
// Everything the collector must see is a Value in the payload, so the generic
// object scan forwards the table without this file owning a root source. The
// hash INDEX is the exception and is deliberately RawBytes-tagged: it holds
// entry indices, not references, and the collector must not read it as Values.
namespace CollectionSlot {
enum : uint32_t {
    Brand = 0,
    Entries,
    Index,
    LiveCount,
    UsedCount,
    IndexEpoch,
    IndexAnchor,
    kCount,
};
}  // namespace CollectionSlot

struct MapHeader {
    ObjectHeader object;
    HeapValue inlineSlots[ObjectHeader::kInlineSlots];
    // The kind's symbol (see above). First, so the brand test reads one slot.
    HeapValue brand;
    // Object-tagged block of Values, two per entry slot: key then value, in
    // INSERTION order. A removed entry's key is the Hole singleton, which is
    // internal by construction and therefore cannot collide with a key a
    // program can hold. Erasing from the middle would move every later entry
    // and break the iteration order a live iterator is holding a cursor into,
    // so a delete tombstones instead.
    HeapValue entries;
    // RawBytes block of uint32 buckets, open-addressed with linear probing.
    // 0 means empty; anything else is an entry slot index plus one.
    HeapValue index;
    // double: entries a program can see. For a WeakMap or WeakSet an upper
    // bound — the collector tombstones a dead key's pair without touching it —
    // re-derived exactly whenever the table is reindexed or regrown.
    HeapValue liveCount;
    HeapValue usedCount;   // double: entry slots handed out, tombstones included
    // double: `Heap::relocation_epoch()` when `index` was last built, or -2
    // when no key it hashes by address could move then (map.cpp,
    // kStableIndex). An object key hashes by ADDRESS and the collector moves
    // young objects, so a relocation invalidates a young key's bucket.
    // Rebuilding lazily on the next lookup is what keeps that from being a
    // correctness bug rather than a cost; a map whose keys are all old is
    // never rebuilt for it.
    HeapValue indexEpoch;
    // double: this map's OWN address when `index` was last built, as an
    // independent witness that nothing has moved. It is here because the
    // epoch, however carefully placed, is a number some future collector
    // could forget to touch — and this one it cannot forget: to relocate any
    // object a collector must trace from the roots, a live map is on that
    // trace, and moving the map changes this. Neither check subsumes the
    // other (an epoch catches a map that happened to land back on its old
    // address; the anchor catches a collector that bypassed the epoch), so
    // the index is valid only when BOTH agree.
    HeapValue indexAnchor;

    // A fresh, empty table on an object of `shape` — which decides its
    // [[Prototype]], and which every caller takes from the constructor's
    // `instance_shape` so that `new Map()` and `new (class extends Map)()`
    // differ in nothing but that. `brand` is the kind's symbol.
    static MapHeader* create(Heap& heap, NonMovingArena& arena, Shape* shape, Value brand);

    // Is `v` an object created by `create` with this brand? Three loads and
    // no allocation: the object kind, the slot count (an ordinary object with
    // this prototype was not built with the slots and answers 0 here, which is
    // what keeps the reads below in bounds), and the brand slot's identity.
    // A brand that is not yet a symbol — the kind's intrinsics unbuilt — can
    // match nothing, since no instance of that kind can exist before them.
    static bool hasBrand(Value v, Value brand) noexcept {
        if (!brand.isSymbol() || !v.isObject()) return false;
        HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
        if (hdr->flags != HeapKind::Plain) return false;
        const auto* obj = reinterpret_cast<const ObjectHeader*>(hdr);
        if (obj->internalSlotCount() != CollectionSlot::kCount) return false;
        return obj->slotsData()[ObjectHeader::kInlineSlots + CollectionSlot::Brand].rawBits() ==
               brand.rawBits();
    }

    uint32_t liveSize() const noexcept { return static_cast<uint32_t>(liveCount.asNumber()); }
    uint32_t used() const noexcept { return static_cast<uint32_t>(usedCount.asNumber()); }
    // How many entry slots the current block holds, derived from the block
    // rather than stored: two facts that can disagree are one fact too many.
    uint32_t capacity() const noexcept;

    HeapValue* entryData() noexcept {
        if (!entries.isPointer()) return nullptr;
        return entries.asObject<HeapObjectHeader>()->payload<HeapValue>();
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

    // The allocation-free half of `find`, for a lookup that wants to skip the
    // rooted prologue entirely (seam: BRONZE_NO_MAP_FAST=1, checked by the
    // CALLER so the seam covers the whole fast prologue). Answers true — with
    // `slot` set to the entry index or UINT32_MAX for a miss — only when the
    // probe provably cannot allocate: the map is empty, or the index is
    // present and both its epoch and its anchor still agree that nothing has
    // moved since it was built. Answers false when only the reindexing `find`
    // can say, and the caller falls back to the rooted path. Raw pointers are
    // safe exactly BECAUSE nothing in here allocates.
    static bool findFast(const Heap& heap, MapHeader* map, Value key, uint32_t& slot) noexcept;

    // Insert or update. Updating an EXISTING key keeps its position, which is
    // 24.1.3.9 step 4 and the reason the table is ordered at all.
    static void set(Heap& heap, Rooted<Value>& self, Rooted<Value>& key, Rooted<Value>& val);

    // True when something was removed. Tombstones the slot; the key and value
    // are dropped so the collector stops holding them live.
    static bool remove(Heap& heap, Rooted<Value>& self, Rooted<Value>& key);

    static void clear(Rooted<Value>& self);
};

// The struct above IS the internal-slot layout `createWithInternalSlots`
// produces, field for field, which is what lets the table's code address its
// fields by name while everything else in the runtime addresses them as slots.
static_assert(offsetof(MapHeader, brand) ==
              sizeof(ObjectHeader) + (ObjectHeader::kInlineSlots + CollectionSlot::Brand) * sizeof(Value));
static_assert(offsetof(MapHeader, entries) ==
              sizeof(ObjectHeader) + (ObjectHeader::kInlineSlots + CollectionSlot::Entries) * sizeof(Value));
static_assert(offsetof(MapHeader, indexAnchor) ==
              sizeof(ObjectHeader) + (ObjectHeader::kInlineSlots + CollectionSlot::IndexAnchor) * sizeof(Value));
static_assert(sizeof(MapHeader) ==
              sizeof(ObjectHeader) + (ObjectHeader::kInlineSlots + CollectionSlot::kCount) * sizeof(Value));

// ECMA-262 7.2.10 SameValueZero: `===` except that NaN matches NaN. `+0` and
// `-0` match under both, which `==` on the raw bits would get wrong, and two
// distinct string objects with the same characters are one key, which a bit
// compare would get wrong the other way.
bool sameValueZero(Value a, Value b) noexcept;

// ECMA-262 7.2.11 SameValue: the relation above with the one correction that
// separates it from SameValueZero — `+0` and `-0` are NOT the same value. It
// lives beside its sibling because the two differ over nothing but the zeroes,
// and every caller that compares a STORED value against a proposed one wants
// this one: 10.1.6.3's redefinition test and 10.5's proxy invariants both.
bool sameValue(Value a, Value b) noexcept;

}  // namespace bronze
