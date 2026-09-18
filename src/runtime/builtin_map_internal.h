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
// and 24.2.5 the iterator objects. The receiver test is here because all three
// ask it — a method of its `this`, the constructor of the object `new` handed
// it, the iterator of the collection it walks — and three spellings of "is
// this a Map or a Set" is how they would come to disagree.

namespace bronze::runtime {

// A Map or a Set — the two kinds the shared method bodies accept. A WeakMap
// and a WeakSet reuse the table under their own kinds and are refused here,
// which is what keeps `Map.prototype.get.call(weakMap, k)` a TypeError.
bool rtIsMapOrSet(Value v);
bool rtIsSetKind(Value v);

// The receiver check every method opens with. `undefined` is not "no
// arguments": a detached `const g = m.get; g(1)` reaches a method with no map
// at all, and answering as though it had one would be a silent wrong answer.
// False with the TypeError already raised.
bool rtRequireMapOrSet(Value self, const char* method);

// 24.1.5.1's [[MapIterationKind]], in the values the slot stores. Pinned to
// the ABI's iterator-kind bits by builtin_map_iterator.cpp.
enum MapIterKind : uint32_t { MapIterKeys = 0, MapIterValues = 1, MapIterEntries = 2 };

// A fresh iterator over `map` of that kind (24.1.5.1 CreateMapIterator /
// 24.2.5.1 CreateSetIterator). ALLOCATES; `map` arrives rooted.
Value rtMakeMapIterator(Rooted<Value>& map, uint32_t kind);

// The method bodies, so the tables in builtin_map.cpp can name them. `mapHas`,
// `mapDelete`, `mapClear`, `mapForEach`, `mapValues` and `mapEntries` serve
// both a Map and a Set, since the two share one table layout.
uint64_t rtMapGetBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapSetBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtSetAddBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapHasBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapDeleteBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapClearBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapForEachBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapKeysBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapValuesBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapEntriesBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

}  // namespace bronze::runtime
