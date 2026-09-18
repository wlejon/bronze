#pragma once

#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/value.h"

// What the three Map/Set translation units share and nothing else may see:
// builtin_map.cpp (the constructors and the member tables),
// builtin_map_methods.cpp (the method bodies) and builtin_map_iterator.cpp
// (the iterator objects `keys()` / `values()` / `entries()` hand back).
//
// One family, three files, along the seam ECMA-262 itself draws: 24.1.1 and
// 24.2.1 are the constructors, 24.1.3 and 24.2.3 the prototype members, 24.1.5
// and 24.2.5 the iterator objects. The receiver test all three ask — a method
// of its `this`, the constructor of the object `new` handed it, the iterator
// of the collection it walks — is `rtIsMapOrSet` in rt_builtins.h, because
// the printer and the for-of cursor ask it too.

namespace bronze::runtime {

// The receiver check every method opens with: RequireInternalSlot(this,
// [[MapData]]) for a Map member, [[SetData]] for a Set member. `undefined` is
// not "no arguments": a detached `const g = m.get; g(1)` reaches a method with
// no map at all, and answering as though it had one would be a silent wrong
// answer — and a Set handed to a Map member is the other wrong receiver, which
// shares the layout and must still fail. False with the TypeError already
// raised.
bool rtRequireCollection(Value self, bool isSet, const char* method);

// 24.1.5.1's [[MapIterationKind]], in the values the slot stores. Pinned to
// the ABI's iterator-kind bits by builtin_map_iterator.cpp.
enum MapIterKind : uint32_t { MapIterKeys = 0, MapIterValues = 1, MapIterEntries = 2 };

// A fresh iterator over `map` of that kind (24.1.5.1 CreateMapIterator /
// 24.2.5.1 CreateSetIterator). ALLOCATES; `map` arrives rooted.
Value rtMakeMapIterator(Rooted<Value>& map, uint32_t kind);

// The method bodies, so the tables in builtin_map.cpp can name them. A Map
// and a Set share one table layout, so the Set bodies are the Map bodies'
// implementations under a second code pointer with the Set brand check —
// distinct function objects, as 24.2.3 has them (builtin_map_methods.cpp says
// why that matters). `Set.prototype.keys` is `Set.prototype.values`, so there
// is no Set keys body.
uint64_t rtMapGetBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapSetBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapHasBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapDeleteBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapClearBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapForEachBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapKeysBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapValuesBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapEntriesBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtSetAddBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtSetHasBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtSetDeleteBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtSetClearBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtSetForEachBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtSetValuesBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtSetEntriesBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

}  // namespace bronze::runtime
