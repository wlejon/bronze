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
extern "C" uint32_t bronze_register_key_string_len(const char* str, size_t len);

// The inline-cache site for a KEY, for a property read whose call site brought
// no entry of its own. The brass backend passes `bronze_prop_get` a null entry
// at every site, so without this every `o.method()` on a plain receiver walks
// the receiver's shape and its whole prototype chain — three.js's renderer
// pays that per property per object per frame. One site per (thread, key),
// polymorphic across every receiver shape the key is read on, with the same
// four ways, the same fill and the same validity (receiver shape, prototype
// epoch, `cachedProtoHolder`) as a call site's entry: the site is the same
// struct, and the property walk fills and consults it through the same code.
// Per thread because a site holds a `Shape*`, and shapes are a thread's.
// Null when BRONZE_NO_KEY_IC=1 lowered the seam. Never moves once handed out.
struct InlineCacheSite* rtKeyCacheSite(uint32_t index);

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
// Ending an epoch returns the thread to "no bracket" if that epoch is the
// current one; spans registered after the entry has returned are then
// permanent rather than tagged with a module they do not belong to.
uint64_t rtBeginModuleEpoch();
void rtEndModuleEpoch(uint64_t epoch);
void rtDropModuleEpoch(uint64_t epoch);

// Invalidate cached global cells across all modules on host global registration and realm switches.
void rtInvalidateGlobalCaches();

// BRONZE_NO_ENV_METHOD_IC=1 (read where every other seam is, heap.cpp's TLS
// init) gates the INSTALL of the two env-capable method-IC forms — the
// env-carrying direct latch and the own-slot latch — and nothing else, so one
// binary A/Bs the mechanism: with the seam down the helper latches exactly
// the env-free entries it always did, and the generated hit path, which
// handles all three forms unconditionally, simply never sees the new two.
void rtSetEnvMethodIcEnabled(bool enabled);
bool rtEnvMethodIcEnabled() noexcept;

// BRONZE_NO_EXOTIC_METHOD_IC=1 (read beside the seam above) gates the INSTALL
// of the exotic-receiver method-IC form — the kind-guarded direct entry an
// Array or collection (Map/Set/WeakMap/WeakSet) receiver latches — and nothing
// else. Latch-side only, for the same reason the env seam is: the generated
// hit path's exotic arm can only match an entry this latch wrote, so with the
// seam down the arm is dead and the helper serves every exotic receiver, which
// is exactly the pre-mechanism behaviour.
void rtSetExoticMethodIcEnabled(bool enabled);
bool rtExoticMethodIcEnabled() noexcept;

// BRONZE_NO_POLY_METHOD_IC=1 gates way-1 DISPLACEMENT at the method-call
// latch — with it down, an overwritten way-0 entry is simply lost, exactly
// the pre-poly behaviour — and nothing else: the generated way-1 compare can
// only match a shape the displacement wrote, so an empty way 1 keeps the arm
// dead. Latch-side for the same one-binary A/B reason the two seams above.
void rtSetPolyMethodIcEnabled(bool enabled);
bool rtPolyMethodIcEnabled() noexcept;

}  // namespace bronze::runtime
