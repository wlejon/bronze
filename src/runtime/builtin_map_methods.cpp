// The members of `Map.prototype` (ECMA-262 24.1.3) and `Set.prototype`
// (24.2.3): the bodies. The tables that name them and the constructors that
// build the receivers they read are builtin_map.cpp; the iterator objects
// three of them hand back are builtin_map_iterator.cpp.
//
// A Map and a Set share one table (map.h says why), so `has`, `delete`,
// `clear`, `forEach`, `values` and `entries` are one body each, dispatching on
// the receiver's kind only where the two answer differently — a Set's
// `forEach` passes the element twice, its `values` reads the key column.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/builtin_map_internal.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/map.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/tls_block.h"
#include "runtime/value.h"

namespace bronze::runtime {

bool rtRequireMapOrSet(Value self, const char* method) {
    if (rtIsMapOrSet(self)) return true;
    rtThrowTypeError("Method " + std::string(method) + " called on an incompatible receiver");
    return false;
}

uint64_t rtMapGetBody(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    // The allocation-free prologue (seam: BRONZE_NO_MAP_FAST=1): a hit or a
    // miss against a valid index runs no allocation anywhere, so the rooted
    // copy of the arguments below defends nothing on this path. A stale or
    // absent index — and every receiver the brand check refuses — falls
    // through to the full path and its exact answers.
    if (rtTls()->map_fast_enabled != 0 && argc >= 1) {
        Value selfV{Value(thisBits)};
        if (rtIsMapOrSet(selfV)) {
            auto* map = selfV.asObject<MapHeader>();
            uint32_t slot;
            if (MapHeader::findFast(rtHeap(), map, Value(argv[0]), slot)) {
                if (slot == UINT32_MAX) return Value::fromUndefined().rawBits();
                return map->valueAt(slot).rawBits();
            }
        }
    }
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!rtRequireMapOrSet(self.get(), "get")) return Value::fromUndefined().rawBits();
    Rooted<Value> key{args[0]};
    const uint32_t slot = MapHeader::find(rtHeap(), self, key);
    if (slot == UINT32_MAX) return Value::fromUndefined().rawBits();
    return self.get().asObject<MapHeader>()->valueAt(slot).rawBits();
}

uint64_t rtMapSetBody(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!rtRequireMapOrSet(self.get(), "set")) return Value::fromUndefined().rawBits();
    Rooted<Value> key{args[0]};
    Rooted<Value> val{args[1]};
    MapHeader::set(rtHeap(), self, key, val);
    return self.get().rawBits();  // 24.1.3.9 returns the map, so `.set` chains
}

uint64_t rtSetAddBody(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!rtRequireMapOrSet(self.get(), "add")) return Value::fromUndefined().rawBits();
    Rooted<Value> key{args[0]};
    // 24.2.3.1: an element already present keeps its POSITION, which falls
    // out of MapHeader::set updating in place rather than re-inserting.
    MapHeader::set(rtHeap(), self, key, key);
    return self.get().rawBits();
}

uint64_t rtMapHasBody(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    // As the `get` fast prologue: no allocation on a valid-index probe, so no
    // roots. Seam: BRONZE_NO_MAP_FAST=1.
    if (rtTls()->map_fast_enabled != 0 && argc >= 1) {
        Value selfV{Value(thisBits)};
        if (rtIsMapOrSet(selfV)) {
            auto* map = selfV.asObject<MapHeader>();
            uint32_t slot;
            if (MapHeader::findFast(rtHeap(), map, Value(argv[0]), slot)) {
                return Value::fromBool(slot != UINT32_MAX).rawBits();
            }
        }
    }
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!rtRequireMapOrSet(self.get(), "has")) return Value::fromUndefined().rawBits();
    Rooted<Value> key{args[0]};
    return Value::fromBool(MapHeader::find(rtHeap(), self, key) != UINT32_MAX).rawBits();
}

uint64_t rtMapDeleteBody(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!rtRequireMapOrSet(self.get(), "delete")) return Value::fromUndefined().rawBits();
    Rooted<Value> key{args[0]};
    return Value::fromBool(MapHeader::remove(rtHeap(), self, key)).rawBits();
}

uint64_t rtMapClearBody(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!rtRequireMapOrSet(self.get(), "clear")) return Value::fromUndefined().rawBits();
    MapHeader::clear(self);
    return Value::fromUndefined().rawBits();
}

uint64_t rtMapForEachBody(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!rtRequireMapOrSet(self.get(), "forEach")) return Value::fromUndefined().rawBits();
    Rooted<Value> cb{args[0]};
    if (!cb.get().isObject() ||
        cb.get().asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return rtThrowTypeError("Map.prototype.forEach needs a function argument").rawBits();
    }
    Rooted<Value> thisArg{args[1]};
    const bool set = rtIsSetKind(self.get());
    // The bound is re-read every step: 24.1.3.5 visits entries added DURING
    // the walk, which is the one place a Map's iteration is not a snapshot.
    for (uint32_t at = 0; at < self.get().asObject<MapHeader>()->used(); ++at) {
        auto* map = self.get().asObject<MapHeader>();
        if (!map->liveAt(at)) continue;
        Value block[3] = {set ? map->keyAt(at) : map->valueAt(at), map->keyAt(at), self.get()};
        cb.get().asObject<FunctionHeader>()->call(thisArg.get(), 3, block);
        // A callback that threw stops the walk, for the reason every callback
        // loop in builtin_array.cpp does.
        if (rtExceptionPending()) break;
    }
    return Value::fromUndefined().rawBits();
}

uint64_t rtMapKeysBody(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!rtRequireMapOrSet(self.get(), "keys")) return Value::fromUndefined().rawBits();
    return rtMakeMapIterator(self, MapIterKeys).rawBits();
}

uint64_t rtMapValuesBody(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!rtRequireMapOrSet(self.get(), "values")) return Value::fromUndefined().rawBits();
    return rtMakeMapIterator(self, MapIterValues).rawBits();
}

uint64_t rtMapEntriesBody(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!rtRequireMapOrSet(self.get(), "entries")) return Value::fromUndefined().rawBits();
    return rtMakeMapIterator(self, MapIterEntries).rawBits();
}

}  // namespace bronze::runtime
