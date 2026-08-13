// `WeakMap` and `WeakSet` (ECMA-262 24.3, 24.4) — the constructors and the
// four methods each defines: get/set/has/delete, add/has/delete.
//
// The storage is builtin_map.cpp's table under two kinds of its own
// (MapHeader::kWeakMapFlags / kWeakSetFlags), and the references it holds are
// STRONG. That is observably correct today, and the reason is the shape of the
// API rather than an accident: a WeakMap is non-iterable, has no `size`, and
// answers only about keys the asker is still holding — so a key kept alive by
// the table is indistinguishable from one kept alive by the program, except
// through memory exhaustion. What strong references cost is exactly that:
// entries whose keys became garbage are never reclaimed. True weakness hangs
// on the Heap's post-collection hook (heap.h, `set_post_collection_hook`) —
// the one window in which a dead key's header is still distinguishable from a
// live key's forwarded one — and lands there when a workload needs it.
//
// The JS surface follows builtin_map.cpp line for line: methods are ordinary
// function objects handed out by the property path, there is no
// WeakMap.prototype OBJECT (`WeakMap.prototype` is a named refusal, and
// `wm instanceof WeakMap` is false — the divergence recorded at the top of
// builtin_map.cpp, inherited deliberately rather than re-decided here).
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
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/map.h"
#include "runtime/rt_internal.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isWeakCollection(Value v) {
    if (!v.isObject()) return false;
    const uint16_t f = v.asObject<HeapObjectHeader>()->flags;
    return f == MapHeader::kWeakMapFlags || f == MapHeader::kWeakSetFlags;
}

// The receiver check every method opens with, for the reason builtin_map.cpp's
// does: a detached `const g = wm.get; g(k)` arrives with no collection at all,
// and answering as though it had one would be a silent wrong answer.
bool requireWeakCollection(Value self, const char* method) {
    if (isWeakCollection(self)) return true;
    rtThrowTypeError("Method " + std::string(method) + " called on an incompatible receiver");
    return false;
}

// 4.2.1 CanBeHeldWeakly: an object, or a symbol with no entry in the global
// symbol registry. Everything else can be re-created from what it IS, so
// holding it weakly would be holding it forever under another name.
bool canBeHeldWeakly(Value v) {
    if (v.isObject()) return true;
    if (v.isSymbol()) return rtSymbolKeyFor(v).isUndefined();
    return false;
}

// ---- the methods ------------------------------------------------------------

uint64_t weakMapGet(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireWeakCollection(self.get(), "get")) return Value::fromUndefined().rawBits();
    // 24.3.3.3 step 4: a key that cannot be held weakly is not an error here —
    // it simply is not in the table, and `undefined` says so.
    if (!canBeHeldWeakly(args[0])) return Value::fromUndefined().rawBits();
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
    if (!canBeHeldWeakly(args[0])) {
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
    if (!canBeHeldWeakly(args[0])) {
        return rtThrowTypeError("Invalid value used in weak set").rawBits();
    }
    Rooted<Value> key{args[0]};
    MapHeader::set(rtHeap(), self, key, key);
    return self.get().rawBits();
}

uint64_t weakMapHas(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireWeakCollection(self.get(), "has")) return Value::fromUndefined().rawBits();
    if (!canBeHeldWeakly(args[0])) return Value::fromBool(false).rawBits();
    Rooted<Value> key{args[0]};
    return Value::fromBool(MapHeader::find(rtHeap(), self, key) != UINT32_MAX).rawBits();
}

uint64_t weakMapDelete(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireWeakCollection(self.get(), "delete")) return Value::fromUndefined().rawBits();
    if (!canBeHeldWeakly(args[0])) return Value::fromBool(false).rawBits();
    Rooted<Value> key{args[0]};
    return Value::fromBool(MapHeader::remove(rtHeap(), self, key)).rawBits();
}

// ---- the constructors -------------------------------------------------------

// `new WeakMap(iterable)` / `new WeakSet(iterable)` — 24.3.1.1 / 24.4.1.1.
// The iterable is walked through the same protocol Map's constructor uses;
// what differs is only the per-item validation above.
uint64_t buildWeakCollection(Rooted<Value>& arg, uint16_t flags) {
    Rooted<Value> self{Value::fromObject(MapHeader::create(rtHeap(), flags))};
    if (arg.get().isUndefined() || arg.get().isNull()) return self.get().rawBits();

    Rooted<Value> rec{Value(bronze_iter_open(arg.get().rawBits()))};
    if (rtExceptionPending()) return self.get().rawBits();
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> item{Value(bronze_iter_value(rec.get().rawBits()))};
        if (flags == MapHeader::kWeakSetFlags) {
            if (!canBeHeldWeakly(item.get())) {
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
            if (!canBeHeldWeakly(k.get())) {
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

uint64_t weakMapConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> arg{args[0]};
    return buildWeakCollection(arg, MapHeader::kWeakMapFlags);
}

uint64_t weakSetConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> arg{args[0]};
    return buildWeakCollection(arg, MapHeader::kWeakSetFlags);
}

struct Method {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const Method kWeakMapMethods[] = {
    {"get", weakMapGet, 1},
    {"set", weakMapSet, 2},
    {"has", weakMapHas, 1},
    {"delete", weakMapDelete, 1},
};

const Method kWeakSetMethods[] = {
    {"add", weakSetAdd, 1},
    {"has", weakMapHas, 1},
    {"delete", weakMapDelete, 1},
};

// Real members of `WeakMap` / `WeakSet` that bronze has not built. `prototype`
// is on both lists for the reason it is on Map's: there is no prototype OBJECT
// here, and `undefined` for it would let a program install a method nothing
// would find. `getOrInsert` / `getOrInsertComputed` are the upsert proposal's,
// listed the day they are standard and not before.
const char* const kWeakMapUnimplemented[] = {
    "constructor", "prototype",
};
const char* const kWeakSetUnimplemented[] = {
    "constructor", "prototype",
};

}  // namespace

Value rtWeakCollectionConstructor(const std::string& name) {
    if (name == "WeakMap") return rtNativeFunction(weakMapConstructor, 0);
    if (name == "WeakSet") return rtNativeFunction(weakSetConstructor, 0);
    return Value::fromUndefined();
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

Value rtWeakCollectionMethod(bool isWeakSetReceiver, const std::string& key) {
    if (isWeakSetReceiver) {
        for (const Method& m : kWeakSetMethods) {
            if (key == m.name) return rtNativeFunction(m.code, m.arity);
        }
        return Value::fromUndefined();
    }
    for (const Method& m : kWeakMapMethods) {
        if (key == m.name) return rtNativeFunction(m.code, m.arity);
    }
    return Value::fromUndefined();
}

// `in`'s half, off the same tables the read path answers from — the one-list
// rule builtin_map.cpp states. No `size` here, and that is 24.3.3 speaking
// rather than a gap: a WeakMap's prototype defines no such accessor.
bool rtWeakCollectionHasMember(bool isWeakSetReceiver, const std::string& key) {
    if (isWeakSetReceiver) {
        for (const Method& m : kWeakSetMethods) {
            if (key == m.name) return true;
        }
    } else {
        for (const Method& m : kWeakMapMethods) {
            if (key == m.name) return true;
        }
    }
    rtCheckWeakCollectionMember(isWeakSetReceiver, key);
    return false;
}

void rtCheckWeakCollectionMember(bool isWeakSetReceiver, const std::string& key) {
    if (isWeakSetReceiver) {
        rtCheckUnimplementedMember("WeakSet", kWeakSetUnimplemented,
                                   std::size(kWeakSetUnimplemented), key);
        return;
    }
    rtCheckUnimplementedMember("WeakMap", kWeakMapUnimplemented,
                               std::size(kWeakMapUnimplemented), key);
}

}  // namespace bronze::runtime
