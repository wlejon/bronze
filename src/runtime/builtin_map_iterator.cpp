// The Map and Set ITERATOR objects (ECMA-262 24.1.5, 24.2.5): what `keys()`,
// `values()` and `entries()` hand back, and the `next` that steps one.
//
// An iterator carries [[IteratedMap]], [[MapNextIndex]] and
// [[MapIterationKind]] as INTERNAL SLOTS — real fields on the object
// (`ObjectHeader::internalSlot`), invisible to every enumeration there is —
// and hangs from %MapIteratorPrototype% / %SetIteratorPrototype%
// (iterator.cpp builds both, each with its @@toStringTag and its chain to
// %IteratorPrototype%, whose `[Symbol.iterator]` is the self-hook).
//
// `for-of` over `map.values()` never calls `next`: iterator.cpp's MapIterator
// record kind steps the slots directly through `rtMapIteratorStep` once
// `rtOpenIterator` has checked the object's own `next` is still the native one.

#include <bit>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_map_internal.h"
#include "runtime/exception.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

static_assert(sizeof(ObjectHeader) + (ObjectHeader::kInlineSlots + MapIteratorSlot::IteratedMap) * sizeof(Value) ==
              BRONZE_ABI_MAP_ITER_SLOT_MAP_OFFSET);
static_assert(sizeof(ObjectHeader) + (ObjectHeader::kInlineSlots + MapIteratorSlot::NextIndex) * sizeof(Value) ==
              BRONZE_ABI_MAP_ITER_SLOT_NEXT_OFFSET);
static_assert(sizeof(ObjectHeader) + (ObjectHeader::kInlineSlots + MapIteratorSlot::Kind) * sizeof(Value) ==
              BRONZE_ABI_MAP_ITER_SLOT_KIND_OFFSET);
static_assert(std::bit_cast<uint64_t>(static_cast<double>(MapIterKeys)) == BRONZE_ABI_MAP_ITER_KIND_KEYS_BITS);
static_assert(std::bit_cast<uint64_t>(static_cast<double>(MapIterValues)) == BRONZE_ABI_MAP_ITER_KIND_VALUES_BITS);
static_assert(std::bit_cast<uint64_t>(static_cast<double>(MapIterEntries)) == BRONZE_ABI_MAP_ITER_KIND_ENTRIES_BITS);

void writeSlot(Rooted<Value>& obj, uint32_t slot, Value val) {
    obj.get().asObject<ObjectHeader>()->setInternalSlot(slot, val);
}

Value makePair(Rooted<Value>& a, Rooted<Value>& b) {
    Rooted<Value> pair{Value(bronze_create_array(2))};
    auto* arr = pair.get().asObject<ArrayHeader>();
    arr->elementsData()[0] = a.get();
    arr->elementsData()[1] = b.get();
    return pair.get();
}

uint64_t mapIterNext(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    // 24.1.5.1 step 3: a receiver without the internal slots is a TypeError,
    // not an exhausted iterator. `rtIsIteratorObject` is how bronze asks "does
    // it have an [[IteratedMap]]" — the kind's prototype and the kind's slots —
    // and it is a memory-safety check as much as a semantic one, since the
    // reads below address fields only an object created here has.
    if (!rtIsIteratorObject(self.get(), IteratorProto::Map) &&
        !rtIsIteratorObject(self.get(), IteratorProto::Set)) {
        return rtThrowTypeError("next called on an incompatible receiver").rawBits();
    }
    Value out = Value::fromUndefined();
    const bool more = rtMapIteratorStep(self, out);
    Rooted<Value> produced{out};
    return rtCreateIterResult(produced, !more).rawBits();
}

}  // namespace

Value rtMakeMapIterator(Rooted<Value>& map, uint32_t kind) {
    // `next` is an OWN property of the iterator rather than a member of
    // %MapIteratorPrototype% — the divergence cases/collection_internal_slots.js
    // records for every iterator kind, and the arrangement the array and
    // string iterators share. The `[Symbol.iterator]` self-hook is INHERITED
    // from %IteratorPrototype% (27.1.2.1), so the object has no own
    // symbol-keyed property at all.
    static thread_local Value s_mapIterNextFn = Value::fromUndefined();
    static thread_local Value s_keyNext = Value::fromUndefined();
    if (s_mapIterNextFn.isUndefined()) {
        s_mapIterNextFn = rtNativeFunction(mapIterNext, 0, "next", 0);
        rtHeap().add_permanent_root(&s_mapIterNextFn);
        s_keyNext = rtMakeString("next");
        rtHeap().add_permanent_root(&s_keyNext);
    }
    const bool set = rtIsSetKind(map.get());
    Rooted<Value> it{rtNewIteratorObject(set ? IteratorProto::Set : IteratorProto::Map)};
    Rooted<Value> nextFn{s_mapIterNextFn};
    Rooted<Value> nk{s_keyNext};
    it.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), nk, nextFn);
    // Written AFTER the property above, which is the only thing here that can
    // allocate: `writeSlot` re-derives the object from its root, so the order
    // is not load-bearing, but reading it in this order is.
    writeSlot(it, MapIteratorSlot::IteratedMap, map.get());
    writeSlot(it, MapIteratorSlot::NextIndex, Value::fromDouble(0.0));
    writeSlot(it, MapIteratorSlot::Kind, Value::fromDouble(static_cast<double>(kind)));
    return it.get();
}

// 24.1.5.1 %MapIteratorPrototype%.next steps 4-12, without the result object.
// `for-of` over `map.values()` steps through here (iterator.cpp's MapIterator
// kind) after `rtOpenIterator` has checked the object's own `next` is still
// `mapIterNext`, so the brand check is the caller's; a foreign receiver never
// arrives.
bool rtMapIteratorStep(Rooted<Value>& self, Value& produced) {
    auto* selfObj = self.get().asObject<ObjectHeader>();
    Value target = selfObj->internalSlot(MapIteratorSlot::IteratedMap);
    if (!rtIsMapOrSet(target)) return false;
    const auto kind = static_cast<uint32_t>(selfObj->internalSlot(MapIteratorSlot::Kind).asNumber());
    uint32_t at = static_cast<uint32_t>(selfObj->internalSlot(MapIteratorSlot::NextIndex).asNumber());

    auto* map = target.asObject<MapHeader>();
    while (at < map->used() && !map->liveAt(at)) ++at;
    if (at >= map->used()) {
        // The cursor is left past the end, so a live iterator over a map that
        // grows after it finished does NOT resume — 24.1.5.1 step 4.c sets
        // [[Map]] to undefined once, and this is that latch.
        selfObj->setInternalSlot(MapIteratorSlot::IteratedMap, Value::fromUndefined());
        return false;
    }
    selfObj->setInternalSlot(MapIteratorSlot::NextIndex,
                             Value::fromDouble(static_cast<double>(at + 1)));

    const bool set = rtIsSetKind(target);
    if (kind == MapIterKeys) {
        produced = map->keyAt(at);
        return true;
    }
    if (kind == MapIterValues) {
        produced = set ? map->keyAt(at) : map->valueAt(at);
        return true;
    }

    // Entries allocates a pair array, so we root values across allocation.
    Rooted<Value> k{map->keyAt(at)};
    Rooted<Value> second{set ? map->keyAt(at) : map->valueAt(at)};
    produced = makePair(k, second);
    return true;
}

bronze_fn_code rtMapIteratorNextCode() { return mapIterNext; }

}  // namespace bronze::runtime
