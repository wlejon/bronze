// `Map` and `Set` — the constructors, the methods, and the iterator objects
// `keys()` / `values()` / `entries()` hand back.
//
// The table itself is map.{h,cpp}; what is here is the JS surface over it.
// The seam is that a Map's METHODS are ordinary bronze function objects
// reached through `bronze_prop_get`, exactly as `Array.prototype`'s are —
// there is no Map.prototype object, because a Map carries no shape and so has
// no prototype link to hang one on. That is a real divergence and is recorded
// as one: `m instanceof Map` is false, and `Map.prototype` is a named error.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isMapLike(Value v) {
    if (!v.isObject()) return false;
    const uint16_t f = v.asObject<HeapObjectHeader>()->flags;
    return f == MapHeader::kMapFlags || f == MapHeader::kSetFlags;
}

bool isSet(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == MapHeader::kSetFlags;
}

// The receiver of a Map or Set method. `undefined` is not "no arguments": a
// detached `const g = m.get; g(1)` reaches here with no map at all, and
// answering as though it had one would be a silent wrong answer.
bool requireMapLike(Value self, const char* method) {
    if (isMapLike(self)) return true;
    rtThrowTypeError("Method " + std::string(method) + " called on an incompatible receiver");
    return false;
}

enum IterKind : uint32_t { Keys = 0, Values = 1, Entries = 2 };

// The iterator object's INTERNAL SLOTS (24.1.5.1): [[IteratedMap]],
// [[MapNextIndex]] and [[MapIterationKind]]. Real fields on the object, which
// is what makes them invisible to every enumeration there is — `Object.keys`,
// `for-in`, spread, `JSON.stringify` AND `getOwnPropertyNames` — rather than
// only to the four defined over enumerable keys.
//
// Neither of these allocates, so neither can move the object; the caller reads
// the pointer out of its root each time regardless, because the code around
// them does allocate.
Value readSlot(Rooted<Value>& obj, uint32_t slot) {
    return obj.get().asObject<ObjectHeader>()->internalSlot(slot);
}

void writeSlot(Rooted<Value>& obj, uint32_t slot, Value val) {
    obj.get().asObject<ObjectHeader>()->setInternalSlot(slot, val);
}

Value makePair(Rooted<Value>& a, Rooted<Value>& b) {
    Rooted<Value> pair{Value(bronze_create_array(2))};
    pair.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, a);
    pair.get().asObject<ArrayHeader>()->setElem(rtHeap(), 1, b);
    return pair.get();
}

// 7.4.1 CreateIterResultObject, in the field order the spec writes it.
Value iterResult(Rooted<Value>& value, bool done) {
    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> vk{rtMakeString("value")};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), vk, value);
    Rooted<Value> dk{rtMakeString("done")};
    Rooted<Value> dv{Value::fromBool(done)};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), dk, dv);
    return out.get();
}

uint64_t mapIterNext(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    // 24.1.5.1 step 3: a receiver without the internal slots is a TypeError,
    // not an exhausted iterator. `rtIsIteratorObject` is how bronze asks "does
    // it have an [[IteratedMap]]" — the kind's prototype and the kind's slots —
    // and it is a memory-safety check as much as a semantic one, since the
    // reads below address fields only an object created here has.
    if (!rtIsIteratorObject(self.get(), IteratorProto::Map)) {
        return rtThrowTypeError("next called on an incompatible receiver").rawBits();
    }
    Rooted<Value> target{readSlot(self, MapIteratorSlot::IteratedMap)};
    if (!isMapLike(target.get())) {
        Rooted<Value> none;
        return iterResult(none, true).rawBits();
    }
    const auto kind = static_cast<uint32_t>(readSlot(self, MapIteratorSlot::Kind).asNumber());
    uint32_t at = static_cast<uint32_t>(readSlot(self, MapIteratorSlot::NextIndex).asNumber());

    auto* map = target.get().asObject<MapHeader>();
    while (at < map->used() && !map->liveAt(at)) ++at;
    if (at >= map->used()) {
        Rooted<Value> none;
        // The cursor is left past the end, so a live iterator over a map that
        // grows after it finished does NOT resume — 24.1.5.1 step 4.c sets
        // [[Map]] to undefined once, and this is that latch.
        writeSlot(self, MapIteratorSlot::IteratedMap, Value::fromUndefined());
        return iterResult(none, true).rawBits();
    }
    Rooted<Value> k{map->keyAt(at)};
    Rooted<Value> v{map->valueAt(at)};
    writeSlot(self, MapIteratorSlot::NextIndex, Value::fromDouble(static_cast<double>(at + 1)));

    Rooted<Value> produced;
    if (kind == Keys) {
        produced.set(k.get());
    } else if (kind == Values) {
        produced.set(isSet(target.get()) ? k.get() : v.get());
    } else {
        Rooted<Value> second{isSet(target.get()) ? k.get() : v.get()};
        produced.set(makePair(k, second));
    }
    return iterResult(produced, false).rawBits();
}

Value makeMapIterator(Rooted<Value>& map, uint32_t kind) {
    // %MapIteratorPrototype% (24.1.5.2), which is where `[Symbol.iterator]`
    // lives — 27.1.2.1 puts the self-hook on the shared %IteratorPrototype%,
    // and an INHERITED property is not an own one, so
    // `Object.getOwnPropertySymbols(m.keys())` is empty. The shape is shared by
    // every map iterator, so the `next` read inside a loop is a monomorphic
    // cache hit.
    Rooted<Value> it{rtNewIteratorObject(IteratorProto::Map)};
    Rooted<Value> nextFn{rtNativeFunction(mapIterNext, 0)};
    Rooted<Value> nk{rtMakeString("next")};
    it.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), nk, nextFn);
    // Written AFTER the property above, which is the only thing here that can
    // allocate: `writeSlot` re-derives the object from its root, so the order
    // is not load-bearing, but reading it in this order is.
    writeSlot(it, MapIteratorSlot::IteratedMap, map.get());
    writeSlot(it, MapIteratorSlot::NextIndex, Value::fromDouble(0.0));
    writeSlot(it, MapIteratorSlot::Kind, Value::fromDouble(static_cast<double>(kind)));
    return it.get();
}

// ---- the methods ------------------------------------------------------------

uint64_t mapGet(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireMapLike(self.get(), "get")) return Value::fromUndefined().rawBits();
    Rooted<Value> key{args[0]};
    const uint32_t slot = MapHeader::find(rtHeap(), self, key);
    if (slot == UINT32_MAX) return Value::fromUndefined().rawBits();
    return self.get().asObject<MapHeader>()->valueAt(slot).rawBits();
}

uint64_t mapSet(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireMapLike(self.get(), "set")) return Value::fromUndefined().rawBits();
    Rooted<Value> key{args[0]};
    Rooted<Value> val{args[1]};
    MapHeader::set(rtHeap(), self, key, val);
    return self.get().rawBits();  // 24.1.3.9 returns the map, so `.set` chains
}

uint64_t setAdd(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireMapLike(self.get(), "add")) return Value::fromUndefined().rawBits();
    Rooted<Value> key{args[0]};
    // 24.2.3.1: an element already present keeps its POSITION, which falls
    // out of MapHeader::set updating in place rather than re-inserting.
    MapHeader::set(rtHeap(), self, key, key);
    return self.get().rawBits();
}

uint64_t mapHas(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireMapLike(self.get(), "has")) return Value::fromUndefined().rawBits();
    Rooted<Value> key{args[0]};
    return Value::fromBool(MapHeader::find(rtHeap(), self, key) != UINT32_MAX).rawBits();
}

uint64_t mapDelete(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireMapLike(self.get(), "delete")) return Value::fromUndefined().rawBits();
    Rooted<Value> key{args[0]};
    return Value::fromBool(MapHeader::remove(rtHeap(), self, key)).rawBits();
}

uint64_t mapClear(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!requireMapLike(self.get(), "clear")) return Value::fromUndefined().rawBits();
    MapHeader::clear(self);
    return Value::fromUndefined().rawBits();
}

uint64_t mapForEach(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireMapLike(self.get(), "forEach")) return Value::fromUndefined().rawBits();
    Rooted<Value> cb{args[0]};
    if (!cb.get().isObject() ||
        cb.get().asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return rtThrowTypeError("Map.prototype.forEach needs a function argument").rawBits();
    }
    Rooted<Value> thisArg{args[1]};
    const bool set = isSet(self.get());
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

uint64_t mapKeys(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!requireMapLike(self.get(), "keys")) return Value::fromUndefined().rawBits();
    return makeMapIterator(self, Keys).rawBits();
}
uint64_t mapValues(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!requireMapLike(self.get(), "values")) return Value::fromUndefined().rawBits();
    return makeMapIterator(self, Values).rawBits();
}
uint64_t mapEntries(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!requireMapLike(self.get(), "entries")) return Value::fromUndefined().rawBits();
    return makeMapIterator(self, Entries).rawBits();
}

// ---- the constructors -------------------------------------------------------

// `new Map(iterable)` and `new Set(iterable)`. The instance `bronze_construct`
// built is discarded: a constructor that returns an object replaces it, which
// is how a native constructor produces a header type of its own.
// `arg` arrives through a ROOT, not by value: creating the collection is an
// allocation, so an iterable held as raw bits would be read after a
// collection had moved it — which is exactly what `new Set([3, 1, 3, 2])`
// did under BRONZE_GC_STRESS=1 before it did.
uint64_t buildCollection(Rooted<Value>& arg, uint16_t flags) {
    Rooted<Value> self{Value::fromObject(MapHeader::create(rtHeap(), flags))};
    if (arg.get().isUndefined() || arg.get().isNull()) return self.get().rawBits();

    Rooted<Value> rec{Value(bronze_iter_open(arg.get().rawBits()))};
    if (rtExceptionPending()) return self.get().rawBits();
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> item{Value(bronze_iter_value(rec.get().rawBits()))};
        if (flags == MapHeader::kSetFlags) {
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
            MapHeader::set(rtHeap(), self, k, v);
        }
        if (rtExceptionPending()) break;
    }
    if (rtExceptionPending()) bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
    return self.get().rawBits();
}

uint64_t mapConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> arg{args[0]};
    return buildCollection(arg, MapHeader::kMapFlags);
}

uint64_t setConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> arg{args[0]};
    return buildCollection(arg, MapHeader::kSetFlags);
}

struct Method {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const Method kMapMethods[] = {
    {"get", mapGet, 1},        {"set", mapSet, 2},         {"has", mapHas, 1},
    {"delete", mapDelete, 1},  {"clear", mapClear, 0},     {"forEach", mapForEach, 1},
    {"keys", mapKeys, 0},      {"values", mapValues, 0},   {"entries", mapEntries, 0},
};

const Method kSetMethods[] = {
    {"add", setAdd, 1},        {"has", mapHas, 1},         {"delete", mapDelete, 1},
    {"clear", mapClear, 0},    {"forEach", mapForEach, 1}, {"keys", mapKeys, 0},
    {"values", mapValues, 0},  {"entries", mapEntries, 0},
};

// Real members of `Map` / `Set` that bronze has not built. `prototype` is on
// both lists deliberately: a Map has no prototype OBJECT here (see the file
// header), and answering `undefined` for it would let a program install a
// method that nothing would ever find.
const char* const kMapUnimplemented[] = {
    "constructor", "groupBy", "prototype",
};
const char* const kSetUnimplemented[] = {
    "constructor",   "difference", "intersection", "isDisjointFrom", "isSubsetOf",
    "isSupersetOf",  "prototype",  "symmetricDifference", "union",
};

}  // namespace

Value rtMapConstructor(const std::string& name) {
    if (name == "Map") return rtNativeFunction(mapConstructor, 0);
    if (name == "Set") return rtNativeFunction(setConstructor, 0);
    return Value::fromUndefined();
}

const char* rtMapConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    if (code == mapConstructor) return "Map";
    if (code == setConstructor) return "Set";
    return nullptr;
}

Value rtMapMethod(bool isSetReceiver, const std::string& key) {
    if (isSetReceiver) {
        for (const Method& m : kSetMethods) {
            if (key == m.name) return rtNativeFunction(m.code, m.arity);
        }
        return Value::fromUndefined();
    }
    for (const Method& m : kMapMethods) {
        if (key == m.name) return rtNativeFunction(m.code, m.arity);
    }
    return Value::fromUndefined();
}

// What `in` must answer about a Map or a Set, off the SAME two tables the read
// path answers from — because the two are one question, and a second list here
// is how they would come to disagree.
//
// A name in neither table is not silently false. `rtCheckMapMember` refuses it
// by name if bronze knows the member and has not built it, which is exactly
// what a READ of that name does: `'constructor' in m` and `m.constructor` are
// both the named hard error, and the day the member lands both answers change
// together. Only a name the read path answers `undefined` for reaches the
// `false` below.
bool rtMapHasMember(bool isSetReceiver, const std::string& key) {
    // 24.1.3.10 / 24.2.3.9 make `size` an accessor on the prototype. `in` does
    // not read it, so the getter bronze has not got costs nothing here — the
    // property exists either way, which is the whole difference between this
    // question and a property read.
    if (key == "size") return true;
    if (isSetReceiver) {
        for (const Method& m : kSetMethods) {
            if (key == m.name) return true;
        }
    } else {
        for (const Method& m : kMapMethods) {
            if (key == m.name) return true;
        }
    }
    rtCheckMapMember(isSetReceiver, key);
    return false;
}

void rtCheckMapMember(bool isSetReceiver, const std::string& key) {
    if (isSetReceiver) {
        rtCheckUnimplementedMember("Set", kSetUnimplemented, std::size(kSetUnimplemented), key);
        return;
    }
    rtCheckUnimplementedMember("Map", kMapUnimplemented, std::size(kMapUnimplemented), key);
}

Value rtMapDefaultIterator(bool isSetReceiver) {
    return rtNativeFunction(isSetReceiver ? mapValues : mapEntries, 0);
}

}  // namespace bronze::runtime
