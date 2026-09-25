// `Map` and `Set` — the two constructors, %Map.prototype% and %Set.prototype%
// (ECMA-262 24.1, 24.2), and the brand that tells an instance of either from
// an object merely built with the same prototype. The method bodies are
// builtin_map_methods.cpp and the iterator objects builtin_map_iterator.cpp;
// builtin_map_internal.h is the seam between the three.
//
// A Map is an ORDINARY OBJECT with internal slots (24.1.4), and bronze builds
// it as one: `MapHeader` is an ObjectHeader carrying [[MapData]] as internal
// slots, `Map.prototype` is a real object chained to `Object.prototype` with
// every 24.1.3 member on it, and an instance reaches `get` by the same
// prototype walk a class instance reaches a method by. Nothing about a Map's
// dispatch is special, which is what lets `m instanceof Map`,
// `Object.getPrototypeOf(m)`, `Map.prototype.get.call(m, k)`, and a subclass
// whose prototype sits between the two all answer as the language says.
//
// The BRAND is a symbol minted here once per kind, held in the first internal
// slot, and never handed to a program. Every method opens by testing it —
// `Map.prototype.get.call({}, k)` and `.call(new Set(), k)` are both the
// TypeError 24.1.3.6 step 2 names — and the constructor tests it to know that
// the object `new` handed it is one it may fill.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/builtin_map_internal.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

static_assert(offsetof(MapHeader, entries) == BRONZE_ABI_MAP_HEADER_ENTRIES_OFFSET);
static_assert(offsetof(MapHeader, usedCount) == BRONZE_ABI_MAP_HEADER_USED_OFFSET);

namespace {

// One record per kind — the two differ in their tables and their brand, and
// in nothing else about how they are assembled. Every field is a permanent
// root (or an arena symbol, which the collector never moves) because the first
// `new Map()` builds all of it and every later one reads it.
struct CollectionIntrinsics {
    Value ctor = Value::fromUndefined();
    Value proto = Value::fromUndefined();
    Value brand = Value::fromUndefined();
    Shape* instanceShape = nullptr;
};

thread_local CollectionIntrinsics g_map;
thread_local CollectionIntrinsics g_set;

void ensureCollectionIntrinsics();

// ---- the constructors -------------------------------------------------------

// `new Map(iterable)` and `new Set(iterable)`. 24.1.1.1 step 2 is
// OrdinaryCreateFromConstructor over NewTarget, and bronze performs it at the
// one allocation site every construction goes through
// (`rtAllocateNativeBaseInstance`) — so `receiver` is ALREADY the collection
// this body has to fill, whether the program wrote `new Map()` or
// `new (class extends Map)()`, and its [[Prototype]] is NewTarget's. Filling
// it in place rather than building one and returning it is what makes the
// derived case work at all: the derived constructor's `this` is that object,
// and its class fields initialize on it after `super()` returns.
//
// A plain CALL — `Map()` — has no receiver and is a TypeError; the check is the
// language's step 1 (NewTarget undefined), read through the only witness the
// uniform calling convention offers. The brand is asked as well, so that
// `Reflect.construct(Map, [], Set)` fills a Map and never a Set.
//
// `arg` arrives through a ROOT, not by value: filling the collection is an
// allocation, so an iterable held as raw bits would be read after a
// collection had moved it — which is exactly what `new Set([3, 1, 3, 2])`
// did under BRONZE_GC_STRESS=1 before it did.
uint64_t buildCollection(Rooted<Value>& receiver, Rooted<Value>& arg, bool isSet) {
    const CollectionIntrinsics& kind = isSet ? g_set : g_map;
    if (!rtIsNativeConstructReceiver(receiver.get()) ||
        !MapHeader::hasBrand(receiver.get(), kind.brand)) {
        return rtThrowTypeError(std::string("Constructor ") + (isSet ? "Set" : "Map") +
                                " requires 'new'")
            .rawBits();
    }
    Rooted<Value> self{receiver.get()};
    if (arg.get().isUndefined() || arg.get().isNull()) return self.get().rawBits();

    // Steps 6-7: `adder` is Get(map, "set") / Get(set, "add"), read ONCE
    // before the walk and refused when it is not callable — it is the
    // receiver's, so a subclass that overrides `add` has its override run per
    // element (`class Tally extends Set { add(v) { ... } }` counts its
    // constructor argument), and `Reflect.construct(Map, [it], Other)` with no
    // `set` on Other.prototype is the TypeError node raises. When the adder
    // is still the intrinsic, the walk stores directly: the intrinsic does
    // nothing a store does not, and the call, the argument block and the
    // brand check it would repeat are what `new Map(bigArray)` is measured on.
    static thread_local StringHeader* const setKey =
        StringHeader::createLatin1InArena(rtArena(), "set", 3);
    static thread_local StringHeader* const addKey =
        StringHeader::createLatin1InArena(rtArena(), "add", 3);
    Rooted<Value> adderKey{Value::fromString(isSet ? addKey : setKey)};
    Rooted<Value> adder{self.get().asObject<ObjectHeader>()->getProp(rtHeap(), adderKey)};
    if (rtExceptionPending()) return self.get().rawBits();
    if (!rtIsCallableValue(adder.get())) {
        return rtThrowTypeError(std::string("'") + (isSet ? "add" : "set") +
                                "' of the new collection is not a function")
            .rawBits();
    }
    const bool direct = adder.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function &&
                        adder.get().asObject<FunctionHeader>()->code ==
                            (isSet ? rtSetAddBody : rtMapSetBody);

    Rooted<Value> rec{Value(bronze_iter_open(arg.get().rawBits()))};
    if (rtExceptionPending()) return self.get().rawBits();
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> item{Value(bronze_iter_value(rec.get().rawBits()))};
        if (isSet) {
            if (direct) {
                MapHeader::set(rtHeap(), self, item, item);
            } else {
                uint64_t block[1] = {item.get().rawBits()};
                bronze_dynamic_call(adder.get().rawBits(), self.get().rawBits(), 1, block);
            }
        } else {
            if (!item.get().isObject()) {
                rtThrowTypeError("Iterator value is not an entry object");
                break;
            }
            Rooted<Value> k{
                Value(bronze_elem_get(item.get().rawBits(), Value::fromDouble(0.0).rawBits()))};
            if (rtExceptionPending()) break;
            Rooted<Value> v{
                Value(bronze_elem_get(item.get().rawBits(), Value::fromDouble(1.0).rawBits()))};
            if (rtExceptionPending()) break;
            if (direct) {
                MapHeader::set(rtHeap(), self, k, v);
            } else {
                uint64_t block[2] = {k.get().rawBits(), v.get().rawBits()};
                bronze_dynamic_call(adder.get().rawBits(), self.get().rawBits(), 2, block);
            }
        }
        if (rtExceptionPending()) break;
    }
    if (rtExceptionPending()) bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
    return self.get().rawBits();
}

uint64_t mapConstructor(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> receiver{Value(thisBits)};
    Rooted<Value> arg{args[0]};
    return buildCollection(receiver, arg, /*isSet=*/false);
}

uint64_t setConstructor(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> receiver{Value(thisBits)};
    Rooted<Value> arg{args[0]};
    return buildCollection(receiver, arg, /*isSet=*/true);
}

// 24.1.3.10 / 24.2.3.9 `get size`: an accessor on the prototype, so
// `Object.getOwnPropertyDescriptor(Map.prototype, "size").get.name` is
// "get size" and a detached getter refuses a foreign receiver — a Set
// included, for the Map's: each is its own function object with its own
// RequireInternalSlot, as the method bodies are.
uint64_t sizeGetter(uint64_t thisBits, bool isSet) {
    const Value self{Value(thisBits)};
    if (!(isSet ? rtIsSetKind(self) : rtIsMapKind(self))) {
        return rtThrowTypeError("Method get size called on an incompatible receiver").rawBits();
    }
    return Value::fromDouble(self.asObject<MapHeader>()->liveSize()).rawBits();
}

uint64_t mapSizeGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    return sizeGetter(thisBits, /*isSet=*/false);
}

uint64_t setSizeGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    return sizeGetter(thisBits, /*isSet=*/true);
}

// ---- the member tables ------------------------------------------------------

// In INSTALL order, which decides the order the prototype's shape chain is
// walked in: `Shape::lookupProperty` reads newest transition first, so the
// members a renderer's inner loop reads per object per frame — `get`, `set`,
// `has`; `add`, `has` — are listed LAST and found first. The names every
// program reads once (`forEach`, `entries`) sit at the far end.
const NativeMethod kMapMethods[] = {
    {"forEach", rtMapForEachBody, 1, 1}, {"entries", rtMapEntriesBody, 0, 0},
    {"keys", rtMapKeysBody, 0, 0},       {"values", rtMapValuesBody, 0, 0},
    {"clear", rtMapClearBody, 0, 0},     {"delete", rtMapDeleteBody, 1, 1},
    {"has", rtMapHasBody, 1, 1},         {"set", rtMapSetBody, 2, 2},
    {"get", rtMapGetBody, 1, 1},
};

// 24.2.3.8: `Set.prototype.keys` IS `Set.prototype.values` — the same function
// object, named "values" — which the code-pointer interning gives for free
// once both rows name `rtSetValuesBody` and "values" is created first. Every
// other row names a SET body: the Map bodies are distinct code pointers, so
// `Map.prototype.has !== Set.prototype.has`, as 24.1.3 and 24.2.3 have it.
const NativeMethod kSetMethods[] = {
    {"forEach", rtSetForEachBody, 1, 1}, {"entries", rtSetEntriesBody, 0, 0},
    {"values", rtSetValuesBody, 0, 0},   {"keys", rtSetValuesBody, 0, 0},
    {"clear", rtSetClearBody, 0, 0},     {"delete", rtSetDeleteBody, 1, 1},
    {"has", rtSetHasBody, 1, 1},         {"add", rtSetAddBody, 1, 1},
};

// ---- assembling the intrinsics ----------------------------------------------

// A symbol-keyed member, on the terms `rtDefineMethods` uses for the
// string-keyed ones: non-enumerable, and a DEFINITION rather than an
// assignment.
void defineSymbolMember(Rooted<Value>& proto, SymbolHeader* sym, Rooted<Value>& val) {
    Rooted<Value> key{Value::fromSymbol(sym)};
    proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                  /*enumerable=*/false, /*defineOwn=*/true);
}

// One prototype and its constructor, in the order the file header explains.
// Publishes each object into `out` the moment it exists, because every
// install below allocates and the collector moves what it has not been told
// to hold.
void buildKind(CollectionIntrinsics& out, bool isSet) {
    Rooted<Value> parent{rtObjectPrototype()};
    Shape* protoShape = rtNewRootShape(parent.get());
    protoShape->used_as_prototype = true;
    ObjectHeader* protoObj = ObjectHeader::create(rtHeap(), rtArena(), protoShape);
    protoObj->header.flags = HeapKind::Plain;
    Rooted<Value> proto{Value::fromObject(protoObj)};
    out.proto = proto.get();
    rtHeap().add_permanent_root(&out.proto);

    // 24.1.3.13 / 24.2.3.12: non-writable, configurable — what
    // `rtDefineToStringTag` defines.
    rtDefineToStringTag(proto, isSet ? "Set" : "Map");

    Rooted<Value> ctor{rtNativeFunction(isSet ? setConstructor : mapConstructor, 0,
                                        isSet ? "Set" : "Map", 0)};
    out.ctor = ctor.get();
    rtHeap().add_permanent_root(&out.ctor);
    {
        // 24.1.3.2 / 24.2.3.3: the back-pointer, a DEFINITION so it does not
        // write through.
        Rooted<Value> key{rtMakeString("constructor")};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ctor, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }

    if (isSet) {
        // 24.2.4's seven set operations, from the table beside their bodies
        // (builtin_set_ops.cpp) — the only members of a Set that read a second
        // collection, and the reason they are a translation unit of their own.
        size_t opCount = 0;
        const NativeMethod* ops = rtSetOperationMethods(opCount);
        rtDefineMethods(proto, ops, opCount);
        rtDefineMethods(proto, kSetMethods, std::size(kSetMethods));
    } else {
        rtDefineMethods(proto, kMapMethods, std::size(kMapMethods));
    }

    {
        // 24.1.3.12 / 24.2.3.11: `[Symbol.iterator]` IS `entries` / `values`,
        // one function object under two keys — the interning on the code
        // pointer answers the same object the table above installed.
        Rooted<Value> iter{isSet ? rtNativeFunction(rtSetValuesBody, 0, "values", 0)
                                 : rtNativeFunction(rtMapEntriesBody, 0, "entries", 0)};
        defineSymbolMember(proto, rtSymbolIterator(), iter);
    }
    {
        Rooted<Value> key{rtMakeString("size")};
        // 10.2.9 step 5: an accessor's getter is named with the "get " prefix.
        Rooted<Value> getter{rtNativeFunction(isSet ? setSizeGetter : mapSizeGetter, 0,
                                              "get size", 0)};
        Rooted<Value> setter{Value::fromUndefined()};
        ObjectHeader::defineAccessor(rtHeap(), rtArena(), proto, key, getter, setter,
                                     /*enumerable=*/false);
    }

    if (!isSet) {
        // 24.1.2.1 `Map.groupBy`, an ordinary own property of the constructor
        // — in the box every function keeps its statics in, where a subclass
        // reaches it by the chain `extends` builds between the boxes. The body
        // is 7.3.35 in builtin_group_by.cpp, shared with `Object.groupBy`.
        rtEnsureFunctionProperties(ctor);
        Rooted<Value> props{ctor.get().asObject<FunctionHeader>()->properties};
        Rooted<Value> key{rtMakeString("groupBy")};
        Rooted<Value> fn{rtNativeFunction(rtMapGroupBy, 2, "groupBy", 2)};
        props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, fn, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }

    // The brand symbol. Minted with no description, because nothing prints it:
    // it exists to be compared by identity and a program can never hold it.
    out.brand = rtMakeSymbol(Value::fromUndefined());

    FunctionHeader* fn = ctor.get().asObject<FunctionHeader>();
    fn->prototype = proto.get();
    // Last: an instance cannot exist before there is a shape to build one
    // from, and `rtAllocateNativeBaseInstance` reads this slot for every
    // construction — the intrinsic's own and a subclass's alike, the latter
    // through the shape `extends` minted for its own prototype.
    fn->instance_shape = rtNewRootShape(proto.get());
    out.instanceShape = fn->instance_shape;
}

void ensureCollectionIntrinsics() {
    if (g_map.proto.isObject()) return;
    buildKind(g_map, /*isSet=*/false);
    buildKind(g_set, /*isSet=*/true);
}

}  // namespace

bool rtIsMapOrSet(Value v) {
    return MapHeader::hasBrand(v, g_map.brand) || MapHeader::hasBrand(v, g_set.brand);
}

bool rtIsMapKind(Value v) { return MapHeader::hasBrand(v, g_map.brand); }

bool rtIsSetKind(Value v) { return MapHeader::hasBrand(v, g_set.brand); }

// By code pointer, as `rtMapConstructorName` is: `Map.prototype[Symbol.iterator]`
// IS `Map.prototype.entries` (24.1.3.12) and `Set.prototype[Symbol.iterator]`
// IS `Set.prototype.values` (24.2.3.11), and `rtNativeFunction` interns each
// body once, so the code is the identity — with no intrinsic built to ask.
bool rtIsIntrinsicCollectionIterator(Value fn, bool isSet) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return false;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    return code == (isSet ? rtSetValuesBody : rtMapEntriesBody);
}

Value rtNewMap() {
    ensureCollectionIntrinsics();
    return Value::fromObject(
        MapHeader::create(rtHeap(), rtArena(), g_map.instanceShape, g_map.brand));
}

Value rtNewSet() {
    ensureCollectionIntrinsics();
    return Value::fromObject(
        MapHeader::create(rtHeap(), rtArena(), g_set.instanceShape, g_set.brand));
}

Value rtNewMapWithShape(Shape* shape, bool isSet) {
    ensureCollectionIntrinsics();
    const CollectionIntrinsics& kind = isSet ? g_set : g_map;
    return Value::fromObject(MapHeader::create(rtHeap(), rtArena(), shape, kind.brand));
}

Value rtMapConstructor(const std::string& name) {
    if (name != "Map" && name != "Set") return Value::fromUndefined();
    ensureCollectionIntrinsics();
    return name == "Map" ? g_map.ctor : g_set.ctor;
}

// By CODE POINTER and never by interning a constructor and comparing bits:
// identifying an intrinsic must not build one, because this is asked from
// paths where an unexpected allocation retires a pointer mid-lookup.
const char* rtMapConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    if (code == mapConstructor) return "Map";
    if (code == setConstructor) return "Set";
    return nullptr;
}

}  // namespace bronze::runtime
