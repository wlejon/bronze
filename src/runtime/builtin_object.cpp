// The `Object` namespace: property descriptors, freezing, and the four
// whole-object copies.
//
// `Object` is a value here, rather than a name the compiler recognises at a
// call. Recognising `Object.keys(...)` at the CALL made every other member a
// compile error, which was
// the right answer while `keys` was the only member; it does not survive a
// second one, because `Object.assign` and `Object.defineProperty` would each
// need their own IL op and their own arity check in lowering. So `Object` joins
// `Math` as an ordinary namespace object resolved by name, and `Object.keys`
// keeps its instruction as a fast path over the SAME C++ function — one
// implementation, so the two spellings cannot drift.
//
// The descriptors themselves: `writable` and `configurable` live in the
// dictionary entry and nowhere else, so asking for either moves the object out
// of its shape chain. That is what keeps the inline caches out of this file
// entirely.

#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/dictionary.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isPlainObject(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN;
}

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == 2;
}

// The arena-interned name a descriptor field or a property key reaches the
// dictionary under. Interning is not an optimization here: a DictEntry
// outlives every collection, so a heap key would dangle.
StringHeader* internOf(Value keyVal) {
    Rooted<Value> str{rtValueToString(keyVal)};
    return StringHeader::internToArena(rtArena(), str.get().asString<StringHeader>());
}

Value readField(Rooted<Value>& desc, const char* name, bool& present) {
    Rooted<Value> key{rtMakeString(name)};
    present = bronze_has_property(key.get().rawBits(), desc.get().rawBits());
    if (!present) return Value::fromUndefined();
    return desc.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
}

void putField(Rooted<Value>& obj, const char* name, Rooted<Value>& val) {
    Rooted<Value> key{rtMakeString(name)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
}

// The entry `name` names, after the object has been moved to dictionary mode.
// Null only for a name that is not an own property.
DictEntry* entryOf(Value objVal, StringHeader* name) {
    auto* obj = objVal.asObject<ObjectHeader>();
    if (!obj->shape || !obj->shape->isDictionary()) return nullptr;
    return obj->shape->dict->find(name);
}

// ECMA-262 10.1.6.3 DefineOwnProperty, for the one caller that can express a
// full descriptor. Every path goes through dictionary mode, including a
// descriptor that asks for nothing unusual: `{ value: 5 }` DEFAULTS its three
// missing attributes to false (6.2.6.5), so the plain-looking case is exactly
// the one a shape transition cannot represent.
uint64_t objectDefineProperty(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) {
        return rtThrowTypeError("Object.defineProperty called on a value that is not an object")
            .rawBits();
    }
    if (!isPlainObject(args[2])) {
        return rtThrowTypeError("Property description must be an object").rawBits();
    }
    Rooted<Value> self{args[0]};
    Rooted<Value> desc{args[2]};

    bool hasValue = false, hasGet = false, hasSet = false;
    bool hasWritable = false, hasEnumerable = false, hasConfigurable = false;
    Rooted<Value> value{readField(desc, "value", hasValue)};
    Rooted<Value> getter{readField(desc, "get", hasGet)};
    Rooted<Value> setter{readField(desc, "set", hasSet)};
    Rooted<Value> writableV{readField(desc, "writable", hasWritable)};
    Rooted<Value> enumerableV{readField(desc, "enumerable", hasEnumerable)};
    Rooted<Value> configurableV{readField(desc, "configurable", hasConfigurable)};

    if ((hasGet || hasSet) && (hasValue || hasWritable)) {
        return rtThrowTypeError(
                   "Invalid property descriptor. Cannot both specify accessors and a value or "
                   "writable attribute")
            .rawBits();
    }
    const bool accessor = hasGet || hasSet;
    const bool writable = hasWritable && bronze_truthy(writableV.get().rawBits());
    const bool enumerable = hasEnumerable && bronze_truthy(enumerableV.get().rawBits());
    const bool configurable = hasConfigurable && bronze_truthy(configurableV.get().rawBits());

    // The key string is built before the object is disturbed, and interned so
    // the entry can hold it forever.
    StringHeader* name = internOf(args[1]);

    ObjectHeader::toDictionary(rtArena(), self);
    DictEntry* existing = entryOf(self.get(), name);
    if (!existing && !self.get().asObject<ObjectHeader>()->shape->dict->extensible) {
        return rtThrowTypeError("Cannot define property, object is not extensible").rawBits();
    }
    if (existing && !existing->configurable) {
        return rtThrowTypeError("Cannot redefine property: " + rtUtf8Chars(name)).rawBits();
    }

    uint32_t slot = 0;
    ObjectHeader* live =
        ObjectHeader::dictDefine(rtHeap(), rtArena(), self, name, enumerable, accessor, slot);
    if (accessor) {
        live->setSlot(slot, hasGet ? getter.get() : Value::fromUndefined());
        live->setSlot(slot + 1, hasSet ? setter.get() : Value::fromUndefined());
    } else {
        live->setSlot(slot, value.get());
    }
    DictEntry* entry = entryOf(self.get(), name);
    entry->writable = writable;
    entry->configurable = configurable;
    return self.get().rawBits();
}

// 6.2.6.4 FromPropertyDescriptor. The FIELD ORDER is the specification's, not
// a convenience — `Object.keys(descriptor)` prints it, so it is pinned bytes.
uint64_t objectGetOwnPropertyDescriptor(uint64_t, uint64_t, uint32_t argc,
                                        const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) {
        return rtThrowTypeError(
                   "Object.getOwnPropertyDescriptor called on a value that is not an object")
            .rawBits();
    }
    Rooted<Value> self{args[0]};
    StringHeader* name = internOf(args[1]);

    PropertyInfo info;
    auto* obj = self.get().asObject<ObjectHeader>();
    if (!obj->shape || !obj->shape->lookupProperty(name, info)) {
        return Value::fromUndefined().rawBits();
    }
    // Read the slots BEFORE building the result object: creating it allocates,
    // and these are raw reads off a pointer a collection would move.
    Rooted<Value> a{obj->getSlot(info.slot)};
    Rooted<Value> b{info.accessor ? obj->getSlot(info.slot + 1) : Value::fromUndefined()};

    Rooted<Value> out{Value(bronze_create_object())};
    if (info.accessor) {
        putField(out, "get", a);
        putField(out, "set", b);
    } else {
        putField(out, "value", a);
        Rooted<Value> w{Value::fromBool(info.writable)};
        putField(out, "writable", w);
    }
    Rooted<Value> e{Value::fromBool(info.enumerable)};
    putField(out, "enumerable", e);
    Rooted<Value> c{Value::fromBool(info.configurable)};
    putField(out, "configurable", c);
    return out.get().rawBits();
}

// 20.1.2.6 SetIntegrityLevel(O, frozen). Every own property loses `writable`
// and `configurable`, and the object loses `[[Extensible]]`. An ACCESSOR
// keeps its halves — there is no `writable` on one — and only stops being
// configurable, which is 10.1.6.3 read literally rather than "make everything
// read-only".
uint64_t objectFreeze(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // 20.1.2.6 step 1 returns a non-object unchanged rather than throwing.
    if (!isPlainObject(args[0])) return args[0].rawBits();
    Rooted<Value> self{args[0]};
    ObjectHeader::toDictionary(rtArena(), self);
    Dictionary& d = *self.get().asObject<ObjectHeader>()->shape->dict;
    for (DictEntry& e : d.entries) {
        if (!e.accessor) e.writable = false;
        e.configurable = false;
    }
    d.extensible = false;
    return self.get().rawBits();
}

uint64_t objectIsFrozen(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // 7.3.15: a non-object is frozen, vacuously.
    if (!isPlainObject(args[0])) return Value::fromBool(true).rawBits();
    auto* obj = args[0].asObject<ObjectHeader>();
    if (!obj->shape || !obj->shape->isDictionary()) return Value::fromBool(false).rawBits();
    const Dictionary& d = *obj->shape->dict;
    if (d.extensible) return Value::fromBool(false).rawBits();
    for (const DictEntry& e : d.entries) {
        if (e.configurable) return Value::fromBool(false).rawBits();
        if (!e.accessor && e.writable) return Value::fromBool(false).rawBits();
    }
    return Value::fromBool(true).rawBits();
}

// 20.1.2.20 Object.seal, which is 20.1.2.6 SetIntegrityLevel(O, sealed):
// `freeze` minus the writable half, so a sealed object's properties can still
// be WRITTEN and cannot be added, removed or redefined.
uint64_t objectSeal(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) return args[0].rawBits();
    Rooted<Value> self{args[0]};
    ObjectHeader::toDictionary(rtArena(), self);
    Dictionary& d = *self.get().asObject<ObjectHeader>()->shape->dict;
    for (DictEntry& e : d.entries) e.configurable = false;
    d.extensible = false;
    return self.get().rawBits();
}

uint64_t objectIsSealed(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) return Value::fromBool(true).rawBits();
    auto* obj = args[0].asObject<ObjectHeader>();
    if (!obj->shape || !obj->shape->isDictionary()) return Value::fromBool(false).rawBits();
    const Dictionary& d = *obj->shape->dict;
    if (d.extensible) return Value::fromBool(false).rawBits();
    for (const DictEntry& e : d.entries) {
        if (e.configurable) return Value::fromBool(false).rawBits();
    }
    return Value::fromBool(true).rawBits();
}

// 20.1.2.19 / 20.1.2.16. `extensible` is a field of the dictionary, so
// clearing it is what moves the object there; asking about it does not.
uint64_t objectPreventExtensions(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) return args[0].rawBits();
    Rooted<Value> self{args[0]};
    ObjectHeader::toDictionary(rtArena(), self);
    self.get().asObject<ObjectHeader>()->shape->dict->extensible = false;
    return self.get().rawBits();
}

uint64_t objectIsExtensible(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) return Value::fromBool(false).rawBits();
    auto* obj = args[0].asObject<ObjectHeader>();
    if (!obj->shape || !obj->shape->isDictionary()) return Value::fromBool(true).rawBits();
    return Value::fromBool(obj->shape->dict->extensible).rawBits();
}

// 20.1.2.12 Object.getPrototypeOf.
//
// bronze has no `Object.prototype`, so a plain `{}` has NO prototype at all —
// where the language says its prototype is that object. Answering `null` for
// it would be a wrong answer that reads exactly like the right one for
// `Object.create(null)`, so the two are kept apart: an explicit null prototype
// answers null, and the absence of a builtin prototype is a named error rather
// than a lie. The same is true of an array and a function, whose prototypes
// are the two other intrinsics bronze does not have.
uint64_t objectGetPrototypeOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) {
        if (!args[0].isObject()) {
            return rtThrowTypeError(
                       "Object.getPrototypeOf called on a value that is not an object")
                .rawBits();
        }
        fatal("unsupported: Object.getPrototypeOf of an array or a function needs "
              "Array.prototype / Function.prototype, which bronze does not provide");
    }
    Shape* shape = args[0].asObject<ObjectHeader>()->shape;
    const Value proto = shape ? shape->prototypeValue() : Value::fromUndefined();
    if (proto.isObject() || proto.isNull()) return proto.rawBits();
    fatal("unsupported: Object.getPrototypeOf of a plain object needs Object.prototype, "
          "which bronze does not provide");
}

// 20.1.2.21 Object.setPrototypeOf. Returns the object, so it composes.
uint64_t objectSetPrototypeOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!args[1].isObject() && !args[1].isNull()) {
        return rtThrowTypeError("Object prototype may only be an Object or null").rawBits();
    }
    if (!isPlainObject(args[0])) {
        if (args[0].isNull() || args[0].isUndefined()) {
            return rtThrowTypeError("Object.setPrototypeOf called on null or undefined")
                .rawBits();
        }
        if (!args[0].isObject()) return args[0].rawBits();  // 20.1.2.21 step 3
        fatal("unsupported: Object.setPrototypeOf on an array, a function, a Map or a Set "
              "(only a plain object carries its prototype on a shape bronze can replace)");
    }
    Rooted<Value> self{args[0]};
    Rooted<Value> proto{args[1]};
    Shape* newRoot = rtRootShapeForPrototype(proto.get());
    ObjectHeader::setPrototype(rtArena(), self, newRoot);
    return self.get().rawBits();
}

// 20.1.2.2 Object.create. `null` really means no prototype, and every walk
// over a prototype chain already stops at a shape whose prototype is not an
// object — so an object with no prototype needs no special case anywhere,
// which is the point of putting the prototype on the shape.
uint64_t objectCreate(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);

// 20.1.2.3 Object.defineProperties, and the loop `Object.create`'s second
// argument shares with it.
bool defineFromDescriptors(Rooted<Value>& target, Rooted<Value>& descriptors) {
    if (!isPlainObject(descriptors.get())) {
        rtThrowTypeError("Property descriptors must be an object");
        return false;
    }
    Rooted<Value> keys{Value(bronze_object_keys(descriptors.get().rawBits()))};
    if (rtExceptionPending()) return false;
    const uint32_t count = keys.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> key{keys.get().asObject<ArrayHeader>()->getElem(i)};
        Rooted<Value> desc{
            Value(bronze_elem_get(descriptors.get().rawBits(), key.get().rawBits()))};
        if (rtExceptionPending()) return false;
        // One implementation of DefineOwnProperty, reached through the same
        // builtin a program would call: `defineProperties` is defined as a
        // loop over `defineProperty` (20.1.2.3.1 step 4) and writing the
        // descriptor decoding twice is how the two would come to disagree
        // about a missing `enumerable`.
        const uint64_t call[3] = {target.get().rawBits(), key.get().rawBits(),
                                  desc.get().rawBits()};
        objectDefineProperty(0, 0, 3, call);
        if (rtExceptionPending()) return false;
    }
    return true;
}

uint64_t objectDefineProperties(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) {
        return rtThrowTypeError("Object.defineProperties called on a value that is not an object")
            .rawBits();
    }
    Rooted<Value> target{args[0]};
    Rooted<Value> descriptors{args[1]};
    defineFromDescriptors(target, descriptors);
    return target.get().rawBits();
}

uint64_t objectCreate(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!args[0].isObject() && !args[0].isNull()) {
        return rtThrowTypeError("Object prototype may only be an Object or null").rawBits();
    }
    if (args[0].isObject() && !isPlainObject(args[0])) {
        fatal("unsupported: Object.create with an array, a function, a Map or a Set as the "
              "prototype (only a plain object may be one)");
    }
    Rooted<Value> proto{args[0]};
    Rooted<Value> out{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtRootShapeForPrototype(proto.get())))};
    out.get().asObject<ObjectHeader>()->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
    if (!args[1].isUndefined()) {
        Rooted<Value> descriptors{args[1]};
        defineFromDescriptors(out, descriptors);
    }
    return out.get().rawBits();
}

// 20.1.2.10 Object.getOwnPropertyNames: own string keys in the same order
// `keys` reports, MINUS the enumerable filter — which is the only difference,
// and is one argument to one walk rather than a second walk.
uint64_t objectGetOwnPropertyNames(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) {
        // An array's own names include `length`, which bronze stores outside
        // the shape system entirely, so the answer would be incomplete rather
        // than merely different.
        if (!args[0].isObject()) {
            return rtThrowTypeError(
                       "Object.getOwnPropertyNames called on a value that is not an object")
                .rawBits();
        }
        fatal("unsupported: Object.getOwnPropertyNames on an array, a function, a Map or a Set "
              "(their own non-enumerable names are not properties bronze stores)");
    }
    Rooted<Value> self{args[0]};
    const std::vector<StringHeader*> ordered =
        rtOwnKeysOrdered(self.get().asObject<ObjectHeader>(), /*enumerableOnly=*/false);
    Rooted<Value> out{Value(bronze_create_array(static_cast<uint32_t>(ordered.size())))};
    uint32_t at = 0;
    for (StringHeader* name : ordered) {
        Rooted<Value> key{rtCopyKeyToHeap(name)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
    }
    return out.get().rawBits();
}

// The three whole-object reads. `keys` is `bronze_object_keys` — the same
// function the IL instruction calls, so the fast path and the namespace
// member can never answer differently.
uint64_t objectKeys(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return bronze_object_keys(args[0].rawBits());
}

// `values` and `entries` share one walk over `keys`, differing only in what
// they put in the output array (20.1.2.24 and 20.1.2.5 are one abstract
// operation, EnumerableOwnProperties, with a `kind`).
uint64_t enumerableOwn(Value source, bool wantEntries) {
    // Rooted before the key walk, not after: `bronze_object_keys` allocates,
    // and a source read back afterwards from raw bits would name where the
    // object used to be.
    Rooted<Value> src{source};
    Rooted<Value> keys{Value(bronze_object_keys(src.get().rawBits()))};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Rooted<Value> out{Value(bronze_create_array(0))};
    const uint32_t count = keys.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> key{keys.get().asObject<ArrayHeader>()->getElem(i)};
        Rooted<Value> val{Value(bronze_elem_get(src.get().rawBits(), key.get().rawBits()))};
        if (rtExceptionPending()) return out.get().rawBits();
        Rooted<Value> item;
        if (wantEntries) {
            Rooted<Value> pair{Value(bronze_create_array(2))};
            pair.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, key);
            pair.get().asObject<ArrayHeader>()->setElem(rtHeap(), 1, val);
            item.set(pair.get());
        } else {
            item.set(val.get());
        }
        const uint32_t at = out.get().asObject<ArrayHeader>()->length;
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at, item);
    }
    return out.get().rawBits();
}

uint64_t objectValues(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return enumerableOwn(args[0], /*wantEntries=*/false);
}

uint64_t objectEntries(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return enumerableOwn(args[0], /*wantEntries=*/true);
}

// 20.1.2.1 Object.assign — CopyDataProperties per source, left to right, so a
// later source wins. The copy is `bronze_object_spread`, which is the same
// operation `{ ...src }` performs; two implementations of "copy the own
// enumerable properties" would be two chances to disagree about a getter.
uint64_t objectAssign(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) {
        return rtThrowTypeError("Object.assign target must be an object").rawBits();
    }
    Rooted<Value> target{args[0]};
    for (uint32_t i = 1; i < args.count(); ++i) {
        Rooted<Value> src{args[i]};
        bronze_object_spread(target.get().rawBits(), src.get().rawBits());
        if (rtExceptionPending()) break;
    }
    return target.get().rawBits();
}

// 20.1.2.7 Object.fromEntries — the inverse of `entries`, and the reason it
// takes an ITERABLE rather than an array: `Object.fromEntries(map)` is how a
// Map becomes a record, and that walk is the protocol.
uint64_t objectFromEntries(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> rec{Value(bronze_iter_open(args[0].rawBits()))};
    if (rtExceptionPending()) return out.get().rawBits();
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> pair{Value(bronze_iter_value(rec.get().rawBits()))};
        if (!pair.get().isObject()) {
            rtThrowTypeError("Iterator value is not an entry object");
            break;
        }
        Rooted<Value> k{Value(bronze_elem_get(pair.get().rawBits(),
                                              Value::fromDouble(0.0).rawBits()))};
        Rooted<Value> v{Value(bronze_elem_get(pair.get().rawBits(),
                                              Value::fromDouble(1.0).rawBits()))};
        bronze_elem_set(out.get().rawBits(), k.get().rawBits(), v.get().rawBits());
        if (rtExceptionPending()) break;
    }
    if (rtExceptionPending()) bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
    return out.get().rawBits();
}

struct NamespaceFn {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const NamespaceFn kObjectFunctions[] = {
    {"keys", objectKeys, 1},
    {"values", objectValues, 1},
    {"entries", objectEntries, 1},
    {"assign", objectAssign, 0},
    {"fromEntries", objectFromEntries, 1},
    {"defineProperty", objectDefineProperty, 3},
    {"getOwnPropertyDescriptor", objectGetOwnPropertyDescriptor, 2},
    {"defineProperties", objectDefineProperties, 2},
    {"freeze", objectFreeze, 1},
    {"isFrozen", objectIsFrozen, 1},
    {"seal", objectSeal, 1},
    {"isSealed", objectIsSealed, 1},
    {"preventExtensions", objectPreventExtensions, 1},
    {"isExtensible", objectIsExtensible, 1},
    {"create", objectCreate, 2},
    {"getPrototypeOf", objectGetPrototypeOf, 1},
    {"setPrototypeOf", objectSetPrototypeOf, 2},
    {"getOwnPropertyNames", objectGetOwnPropertyNames, 1},
};

// Real members of `Object` that bronze has not built. `prototype` is the
// load-bearing one: bronze has no `Object.prototype` object, which is why
// `Object.getPrototypeOf({})` is a named error rather than a `null` that would
// be indistinguishable from `Object.create(null)`'s honest answer.
const char* const kObjectUnimplemented[] = {
    "getOwnPropertyDescriptors",
    "getOwnPropertySymbols",
    "groupBy",
    "hasOwn",
    "is",
    "prototype",
};

Value g_objectNamespace = Value::fromUndefined();

}  // namespace

Value rtObjectNamespace() {
    if (g_objectNamespace.isObject()) return g_objectNamespace;
    // Its own root shape, for the reason `Math` has one: a site reading
    // `Object.keys` must not share a transition tree with `{}` literals.
    Rooted<Value> obj{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(Value::fromUndefined())))};
    obj.get().asObject<ObjectHeader>()->header.flags = 0;
    for (const NamespaceFn& fn : kObjectFunctions) {
        Rooted<Value> key{rtMakeString(fn.name)};
        Rooted<Value> val{Value(bronze_function_singleton(fn.code, fn.arity))};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }
    g_objectNamespace = obj.get();
    rtHeap().add_permanent_root(&g_objectNamespace);
    return g_objectNamespace;
}

void rtObjectCheckMissingMember(Value obj, const std::string& key) {
    if (!g_objectNamespace.isObject() || obj.rawBits() != g_objectNamespace.rawBits()) return;
    rtCheckUnimplementedMember("Object", kObjectUnimplemented, std::size(kObjectUnimplemented),
                               key);
}

}  // namespace bronze::runtime
