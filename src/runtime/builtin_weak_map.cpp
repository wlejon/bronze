// `WeakMap` and `WeakSet` (ECMA-262 24.3, 24.4) — the constructors,
// %WeakMap.prototype% and %WeakSet.prototype%, and the four methods each
// defines: get/set/has/delete, add/has/delete.
//
// The storage is builtin_map.cpp's table under two BRANDS of its own, and the
// references it holds are STRONG. That is observably correct today, and the
// reason is the shape of the API rather than an accident: a WeakMap is
// non-iterable, has no `size`, and answers only about keys the asker is still
// holding — so a key kept alive by the table is indistinguishable from one
// kept alive by the program, except through memory exhaustion. What strong
// references cost is exactly that: entries whose keys became garbage are never
// reclaimed. True weakness hangs on the Heap's post-collection hook (heap.h,
// `add_post_collection_hook`) — the one window in which a dead key's header is
// still distinguishable from a live key's forwarded one — and lands there when
// a workload needs it.
//
// The JS surface follows builtin_map.cpp line for line: an instance is an
// ordinary object with [[WeakMapData]] as internal slots, the prototype is a
// real object with the members on it, and every method opens with the brand
// test — so `WeakMap.prototype.get.call(new Map(), k)` is the TypeError
// 24.3.3.3 step 2 names rather than a read of a Map's entries.
//
// CanBeHeldWeakly (4.2.1): a key may be an object, or a symbol that is NOT in
// the `Symbol.for` registry — a registered symbol can always be re-minted from
// its string, so it can never become unreachable. bronze supports both halves:
// object keys, and unregistered-symbol keys (`rtSymbolKeyFor` answering
// `undefined` is the registry test). A primitive or a registered symbol is the
// TypeError the clauses name for `set`/`add`, and plain `false`/`undefined`
// for `has`/`get`/`delete` (24.3.3.4 step 4 and friends return before
// touching the table).

#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/map.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/symbol.h"
#include "runtime/tls_block.h"
#include "runtime/value.h"
#include "runtime/weak_ref.h"

namespace bronze::runtime {

namespace {

// As builtin_map.cpp's record: one per kind, every object a permanent root.
struct WeakIntrinsics {
    Value ctor = Value::fromUndefined();
    Value proto = Value::fromUndefined();
    Value brand = Value::fromUndefined();
    Shape* instanceShape = nullptr;
};

thread_local WeakIntrinsics g_weakMap;
thread_local WeakIntrinsics g_weakSet;

void ensureWeakIntrinsics();

bool isWeakCollection(Value v) {
    return MapHeader::hasBrand(v, g_weakMap.brand) || MapHeader::hasBrand(v, g_weakSet.brand);
}

// The receiver check every method opens with, for the reason builtin_map.cpp's
// does: a detached `const g = wm.get; g(k)` arrives with no collection at all,
// and answering as though it had one would be a silent wrong answer.
bool requireWeakCollection(Value self, const char* method) {
    if (isWeakCollection(self)) return true;
    rtThrowTypeError("Method " + std::string(method) + " called on an incompatible receiver");
    return false;
}

// ---- the methods ------------------------------------------------------------

uint64_t weakMapGet(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    // The allocation-free prologue (seam: BRONZE_NO_MAP_FAST=1), and the
    // hottest lookup the runtime serves: three.js's renderer keeps its
    // per-object state in a WeakMap it consults per object per frame. An
    // OBJECT key against a valid index probes with no allocation anywhere, so
    // the rooted argument copy defends nothing; a symbol key (whose
    // CanBeHeldWeakly answer consults the registry), a stale index, and every
    // refused receiver fall through to the full path and its exact answers.
    if (rtTls()->map_fast_enabled != 0 && argc >= 1) {
        Value selfV{Value(thisBits)};
        Value keyV{Value(argv[0])};
        if (isWeakCollection(selfV) && keyV.isObject()) {
            auto* map = selfV.asObject<MapHeader>();
            uint32_t slot;
            if (MapHeader::findFast(rtHeap(), map, keyV, slot)) {
                if (slot == UINT32_MAX) return Value::fromUndefined().rawBits();
                return map->valueAt(slot).rawBits();
            }
        }
    }
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireWeakCollection(self.get(), "get")) return Value::fromUndefined().rawBits();
    // 24.3.3.3 step 4: a key that cannot be held weakly is not an error here —
    // it simply is not in the table, and `undefined` says so.
    if (!rtCanBeHeldWeakly(args[0])) return Value::fromUndefined().rawBits();
    Rooted<Value> key{args[0]};
    const uint32_t slot = MapHeader::find(rtHeap(), self, key);
    if (slot == UINT32_MAX) return Value::fromUndefined().rawBits();
    return self.get().asObject<MapHeader>()->valueAt(slot).rawBits();
}

uint64_t weakMapSet(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireWeakCollection(self.get(), "set")) return Value::fromUndefined().rawBits();
    // 24.3.3.5 step 4 throws where `get` above answers `undefined`: storing
    // under a key that can never be collected is the mistake the type exists
    // to prevent, so the write is the loud half.
    if (!rtCanBeHeldWeakly(args[0])) {
        return rtThrowTypeError("Invalid value used as weak map key").rawBits();
    }
    Rooted<Value> key{args[0]};
    Rooted<Value> val{args[1]};
    MapHeader::set(rtHeap(), self, key, val);
    return self.get().rawBits();  // 24.3.3.5 returns the map, so `.set` chains
}

uint64_t weakSetAdd(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireWeakCollection(self.get(), "add")) return Value::fromUndefined().rawBits();
    if (!rtCanBeHeldWeakly(args[0])) {
        return rtThrowTypeError("Invalid value used in weak set").rawBits();
    }
    Rooted<Value> key{args[0]};
    MapHeader::set(rtHeap(), self, key, key);
    return self.get().rawBits();
}

uint64_t weakMapHas(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    // As weakMapGet's fast prologue. Seam: BRONZE_NO_MAP_FAST=1.
    if (rtTls()->map_fast_enabled != 0 && argc >= 1) {
        Value selfV{Value(thisBits)};
        Value keyV{Value(argv[0])};
        if (isWeakCollection(selfV) && keyV.isObject()) {
            auto* map = selfV.asObject<MapHeader>();
            uint32_t slot;
            if (MapHeader::findFast(rtHeap(), map, keyV, slot)) {
                return Value::fromBool(slot != UINT32_MAX).rawBits();
            }
        }
    }
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireWeakCollection(self.get(), "has")) return Value::fromUndefined().rawBits();
    if (!rtCanBeHeldWeakly(args[0])) return Value::fromBool(false).rawBits();
    Rooted<Value> key{args[0]};
    return Value::fromBool(MapHeader::find(rtHeap(), self, key) != UINT32_MAX).rawBits();
}

uint64_t weakMapDelete(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireWeakCollection(self.get(), "delete")) return Value::fromUndefined().rawBits();
    if (!rtCanBeHeldWeakly(args[0])) return Value::fromBool(false).rawBits();
    Rooted<Value> key{args[0]};
    return Value::fromBool(MapHeader::remove(rtHeap(), self, key)).rawBits();
}

// ---- the constructors -------------------------------------------------------

// `new WeakMap(iterable)` / `new WeakSet(iterable)` — 24.3.1.1 / 24.4.1.1.
// The receiver is the object `new` already allocated from NewTarget
// (builtin_map.cpp's `buildCollection` says why filling it in place is what
// makes a subclass work), and the iterable is walked through the same
// protocol; what differs from Map's is only the per-item validation.
uint64_t buildWeakCollection(Rooted<Value>& receiver, Rooted<Value>& arg, bool isWeakSet) {
    const WeakIntrinsics& kind = isWeakSet ? g_weakSet : g_weakMap;
    if (!rtIsNativeConstructReceiver(receiver.get()) ||
        !MapHeader::hasBrand(receiver.get(), kind.brand)) {
        return rtThrowTypeError(std::string("Constructor ") + (isWeakSet ? "WeakSet" : "WeakMap") +
                                " requires 'new'")
            .rawBits();
    }
    Rooted<Value> self{receiver.get()};
    if (arg.get().isUndefined() || arg.get().isNull()) return self.get().rawBits();

    Rooted<Value> rec{Value(bronze_iter_open(arg.get().rawBits()))};
    if (rtExceptionPending()) return self.get().rawBits();
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> item{Value(bronze_iter_value(rec.get().rawBits()))};
        if (isWeakSet) {
            if (!rtCanBeHeldWeakly(item.get())) {
                rtThrowTypeError("Invalid value used in weak set");
                break;
            }
            MapHeader::set(rtHeap(), self, item, item);
        } else {
            if (!item.get().isObject()) {
                rtThrowTypeError("Iterator value is not an entry object");
                break;
            }
            Rooted<Value> k{
                Value(bronze_elem_get(item.get().rawBits(), Value::fromDouble(0.0).rawBits()))};
            Rooted<Value> v{
                Value(bronze_elem_get(item.get().rawBits(), Value::fromDouble(1.0).rawBits()))};
            if (!rtCanBeHeldWeakly(k.get())) {
                rtThrowTypeError("Invalid value used as weak map key");
                break;
            }
            MapHeader::set(rtHeap(), self, k, v);
        }
        if (rtExceptionPending()) break;
    }
    if (rtExceptionPending()) bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
    return self.get().rawBits();
}

uint64_t weakMapConstructor(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> receiver{Value(thisBits)};
    Rooted<Value> arg{args[0]};
    return buildWeakCollection(receiver, arg, /*isWeakSet=*/false);
}

uint64_t weakSetConstructor(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> receiver{Value(thisBits)};
    Rooted<Value> arg{args[0]};
    return buildWeakCollection(receiver, arg, /*isWeakSet=*/true);
}

// In INSTALL order, hot members last (builtin_map.cpp says why): `get` and
// `set` off a WeakMap are three.js's per-object-per-frame reads.
const NativeMethod kWeakMapMethods[] = {
    {"delete", weakMapDelete, 1, 1},
    {"has", weakMapHas, 1, 1},
    {"set", weakMapSet, 2, 2},
    {"get", weakMapGet, 1, 1},
};

const NativeMethod kWeakSetMethods[] = {
    {"delete", weakMapDelete, 1, 1},
    {"has", weakMapHas, 1, 1},
    {"add", weakSetAdd, 1, 1},
};

// ---- assembling the intrinsics ----------------------------------------------

void buildKind(WeakIntrinsics& out, bool isWeakSet) {
    Rooted<Value> parent{rtObjectPrototype()};
    Shape* protoShape = rtNewRootShape(parent.get());
    protoShape->used_as_prototype = true;
    ObjectHeader* protoObj = ObjectHeader::create(rtHeap(), rtArena(), protoShape);
    protoObj->header.flags = HeapKind::Plain;
    Rooted<Value> proto{Value::fromObject(protoObj)};
    out.proto = proto.get();
    rtHeap().add_permanent_root(&out.proto);

    // 24.3.3.6 / 24.4.3.5.
    rtDefineToStringTag(proto, isWeakSet ? "WeakSet" : "WeakMap");

    Rooted<Value> ctor{rtNativeFunction(isWeakSet ? weakSetConstructor : weakMapConstructor, 0,
                                        isWeakSet ? "WeakSet" : "WeakMap", 0)};
    out.ctor = ctor.get();
    rtHeap().add_permanent_root(&out.ctor);
    {
        Rooted<Value> key{rtMakeString("constructor")};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ctor, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
    if (isWeakSet) {
        rtDefineMethods(proto, kWeakSetMethods, std::size(kWeakSetMethods));
    } else {
        rtDefineMethods(proto, kWeakMapMethods, std::size(kWeakMapMethods));
    }
    // No `size` and no `[Symbol.iterator]`, and that is 24.3.3 speaking rather
    // than a gap: non-iterability is half of what makes the weak pair weak.

    out.brand = rtMakeSymbol(Value::fromUndefined());

    FunctionHeader* fn = ctor.get().asObject<FunctionHeader>();
    fn->prototype = proto.get();
    fn->instance_shape = rtNewRootShape(proto.get());
    out.instanceShape = fn->instance_shape;
}

void ensureWeakIntrinsics() {
    if (g_weakMap.proto.isObject()) return;
    buildKind(g_weakMap, /*isWeakSet=*/false);
    buildKind(g_weakSet, /*isWeakSet=*/true);
}

}  // namespace

bool rtIsWeakMapObject(Value v) { return MapHeader::hasBrand(v, g_weakMap.brand); }
bool rtIsWeakSetObject(Value v) { return MapHeader::hasBrand(v, g_weakSet.brand); }

Value rtNewWeakMap() {
    ensureWeakIntrinsics();
    return Value::fromObject(
        MapHeader::create(rtHeap(), rtArena(), g_weakMap.instanceShape, g_weakMap.brand));
}

Value rtNewWeakSet() {
    ensureWeakIntrinsics();
    return Value::fromObject(
        MapHeader::create(rtHeap(), rtArena(), g_weakSet.instanceShape, g_weakSet.brand));
}

Value rtNewWeakCollectionWithShape(Shape* shape, bool isWeakSet) {
    ensureWeakIntrinsics();
    const WeakIntrinsics& kind = isWeakSet ? g_weakSet : g_weakMap;
    return Value::fromObject(MapHeader::create(rtHeap(), rtArena(), shape, kind.brand));
}

Value rtWeakCollectionConstructor(const std::string& name) {
    if (name != "WeakMap" && name != "WeakSet") return Value::fromUndefined();
    ensureWeakIntrinsics();
    return name == "WeakMap" ? g_weakMap.ctor : g_weakSet.ctor;
}

const char* rtWeakCollectionConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    if (code == weakMapConstructor) return "WeakMap";
    if (code == weakSetConstructor) return "WeakSet";
    return nullptr;
}

}  // namespace bronze::runtime
