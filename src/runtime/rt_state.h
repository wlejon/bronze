#pragma once

#include <cstdint>
#include <string>

#include "runtime/heap.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

// The runtime's process-wide state — the heap, the non-moving arena, the root
// shapes and the key registry the index lowering writes into.
//
// All of it is owned by ONE translation unit (rt_state.cpp), so its
// construction order is that unit's business alone. Every other runtime
// translation unit reaches it through the accessors here rather than declaring
// statics of its own, which would put the collector's roots at the mercy of
// cross-TU initialization order.

namespace bronze::runtime {

Heap& rtHeap();
NonMovingArena& rtArena();

// A root shape registered with the collector, for a builtin that needs its
// own hidden class rather than the one every `{}` literal shares.
Shape* rtNewRootShape(Value proto);

// The root shape for objects whose prototype is `proto`, memoized on the
// prototype's identity. `Object.create(p)` in a loop must not mint a hidden
// class per object — every one of them would be a shape no inline cache had
// ever seen, and each would leak an immortal arena shape.
Shape* rtRootShapeForPrototype(Value proto);

// The one root shape every plain `{}` literal starts from. Per-literal root
// shapes would give two identical literals unrelated hidden classes, so a site
// seeing both would miss its inline cache every time.
Shape* rtPlainObjectShape();
Shape* rtCurrentPlainObjectShape();
void rtRegisterRootShape(Shape* shape);

// A property key by the index lowering assigned it. The string form is for
// comparisons; the header form is the arena-interned key the property path
// uses, so a property access allocates nothing. `rtKeyHeader` is null for an
// index no `bronze_register_key_string` call ever covered.
struct KeyInfo {
    uint32_t elemIndex = UINT32_MAX;
    bool isElemIndex = false;
    bool isLength = false;
};

const std::string& rtKeyString(uint32_t index);
StringHeader* rtKeyHeader(uint32_t index);
const KeyInfo& rtKeyInfo(uint32_t index);

uint32_t rtArrayMethodId(const std::string& key);
Value rtArrayMethodById(uint32_t id);
void rtVisitArrayMethodRoots(const Heap::RootVisitor& visit);

// Module load epochs: the unregister half of the module root spans. A host
// (embed.h's beginModuleLoad/unloadModule) brackets a module's entry with an
// epoch; every span the module registers during its entry is tagged with it,
// and dropping the epoch removes those spans — and the function singletons
// interned through the module's own fn slots — from the collector's roots.
// Epoch 0 is "no bracket": spans registered outside any bracket (a linked
// program's, a host that never unloads) are permanent, exactly as before.
uint64_t rtBeginModuleEpoch();
void rtDropModuleEpoch(uint64_t epoch);

}  // namespace bronze::runtime
