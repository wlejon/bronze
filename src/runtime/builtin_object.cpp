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

#include <iterator>
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
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isPlainObject(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN;
}

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == 2;
}

// ToPropertyKey (7.1.19) into the immortal form a DictEntry can hold. Interning
// is not an optimization here: a DictEntry outlives every collection, so a heap
// key would dangle.
//
// A SYMBOL is returned as it stands and never converted: it already lives in
// the arena, and running ToString on one is the TypeError this whole type
// exists to raise. That is also what lets `Object.defineProperty`,
// `getOwnPropertyDescriptor` and `hasOwn` take a symbol key without a branch of
// their own.
PropertyKey internOf(Value keyVal) {
    if (keyVal.isSymbol()) return PropertyKey::fromValue(keyVal);
    Rooted<Value> str{rtValueToString(keyVal)};
    return PropertyKey::forString(
        StringHeader::internToArena(rtArena(), str.get().asString<StringHeader>()));
}

// A key as text, for a diagnostic. `Symbol(desc)` for a symbol, which is the
// only spelling one has (20.4.3.3.1) and is not a conversion a program could
// have performed itself.
std::string keyText(PropertyKey key) {
    return key.isSymbol() ? rtSymbolDescriptiveString(key.toValue()) : rtUtf8Chars(key.string());
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

// The same, for a key that is already a value — a descriptor map's keys are
// the target's own property names, which have no C string to go back to.
void putField(Rooted<Value>& obj, Rooted<Value>& key, Rooted<Value>& val) {
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
}

// Own-property existence, shared by `Object.prototype.hasOwnProperty` and
// `Object.hasOwn` — 20.1.3.2 and 20.1.2.13 are the same operation with the
// receiver in a different position, and writing it twice is how the two would
// come to disagree about a dictionary-mode object.
//
// The key is interned BEFORE the object is read: `internOf` runs ToString,
// which allocates, so an ObjectHeader* taken across it would be stale.
bool hasOwnPropertyNamed(Rooted<Value>& self, Value key) {
    // A String exotic object's own keys are not all in its shape: 10.4.3.4
    // synthesises index properties from the wrapped characters, and bronze
    // answers those on the property path only. Refused by name rather than
    // reported absent (rt_object.cpp says why they are not materialised).
    rtCheckStringExoticOwnKeys(self.get(), "testing");
    PropertyKey name = internOf(key);
    auto* obj = self.get().asObject<ObjectHeader>();
    uint32_t slot = 0;
    return obj->shape && obj->shape->lookupProperty(name, slot);
}

// The entry `name` names, after the object has been moved to dictionary mode.
// Null only for a name that is not an own property.
DictEntry* entryOf(Value objVal, PropertyKey name) {
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

    // The key is built before the object is disturbed, and interned so the
    // entry can hold it forever.
    PropertyKey name = internOf(args[1]);

    ObjectHeader::toDictionary(rtArena(), self);
    DictEntry* existing = entryOf(self.get(), name);
    if (!existing && !self.get().asObject<ObjectHeader>()->shape->dict->extensible) {
        return rtThrowTypeError("Cannot define property, object is not extensible").rawBits();
    }
    if (existing && !existing->configurable) {
        return rtThrowTypeError("Cannot redefine property: " + keyText(name)).rawBits();
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
    rtCheckStringExoticOwnKeys(self.get(), "describing");
    PropertyKey name = internOf(args[1]);

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
// A plain object answers from its shape, and `Object.prototype` is what the
// chain of a `{}` ends at — so `null` here means what the language means by it:
// `Object.create(null)`, and `Object.prototype` itself. The two used to be
// indistinguishable, which is why this was a named error.
//
// An array and a function still are: their members are answered by the
// property path rather than found on a prototype object, so there is no
// `Array.prototype` to return and `null` would be a lie about a chain that
// really does have methods on it.
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
    // A plain object whose root shape carries `undefined` rather than a
    // prototype: every route to one now names either an object or null, so this
    // is a bronze bug and not a program's doing.
    fatal("internal: a plain object whose root shape names no prototype");
}

// 20.1.2.13 Object.hasOwn(O, P) — `hasOwnProperty` with the receiver moved into
// the argument list, and the reason the method form is not the idiom: the
// method can be shadowed by an own property of the object being asked about.
uint64_t objectHasOwn(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) {
        return rtThrowTypeError("Object.hasOwn called on a value that is not an object")
            .rawBits();
    }
    Rooted<Value> self{args[0]};
    return Value::fromBool(hasOwnPropertyNamed(self, args[1])).rawBits();
}

// 20.1.2.14 Object.is — SameValue (7.2.11), which differs from `===` in exactly
// two places and exists for them: NaN is the same value as itself, and +0 is
// not the same value as -0.
uint64_t objectIs(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const Value a = args[0];
    const Value b = args[1];
    if (a.isNumber() && b.isNumber()) {
        const double x = a.asNumber();
        const double y = b.asNumber();
        if (x != x && y != y) return Value::fromBool(true).rawBits();  // both NaN
        // Zeroes differ by sign only, which compares equal as doubles; the bit
        // patterns are what SameValue distinguishes, so they are what is
        // compared. Not a raw rawBits() compare for every pair of numbers,
        // because a NaN has many encodings and two of them are the same value.
        if (x == 0.0 && y == 0.0) {
            return Value::fromBool(std::signbit(x) == std::signbit(y)).rawBits();
        }
        return Value::fromBool(x == y).rawBits();
    }
    // Every other kind: SameValue is SameValueNonNumber (7.2.12), which agrees
    // with strict equality everywhere it is defined — the two clauses differ
    // only over numbers, which the branch above has already taken.
    return Value::fromBool(bronze_strict_eq(a.rawBits(), b.rawBits())).rawBits();
}

// Defined below, and called here: 20.1.2.9 is a loop over the own keys
// `getOwnPropertyNames` collects.
uint64_t objectGetOwnPropertyNames(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);

// 20.1.2.9 Object.getOwnPropertyDescriptors: one entry per own property, each
// the object `getOwnPropertyDescriptor` builds. Defined in terms of it (step 4
// calls it per key), so it calls it, rather than growing a second copy of the
// descriptor shape that could drift from the first.
uint64_t objectGetOwnPropertyDescriptors(uint64_t, uint64_t, uint32_t argc,
                                         const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) {
        return rtThrowTypeError(
                   "Object.getOwnPropertyDescriptors called on a value that is not an object")
            .rawBits();
    }
    Rooted<Value> self{args[0]};
    Rooted<Value> out{Value(bronze_create_object())};
    // ALL own keys, not just the enumerable ones (20.1.2.9 step 2 is
    // OwnPropertyKeys), which is where this differs from `Object.keys`.
    const uint64_t ownCall[1] = {self.get().rawBits()};
    Rooted<Value> names{Value(objectGetOwnPropertyNames(0, 0, 1, ownCall))};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    const uint32_t count = names.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> key{names.get().asObject<ArrayHeader>()->getElem(i)};
        const uint64_t call[2] = {self.get().rawBits(), key.get().rawBits()};
        Rooted<Value> desc{Value(objectGetOwnPropertyDescriptor(0, 0, 2, call))};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        putField(out, key, desc);
    }
    return out.get().rawBits();
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
    // 20.1.2.10 is the STRING half of OwnPropertyKeys — the symbol half is
    // `getOwnPropertySymbols`, and the two are separate functions in the
    // language precisely so that neither ever reports the other's keys.
    const std::vector<StringHeader*> ordered =
        rtOwnStringKeysOrdered(self.get().asObject<ObjectHeader>(), /*enumerableOnly=*/false);
    Rooted<Value> out{Value(bronze_create_array(static_cast<uint32_t>(ordered.size())))};
    uint32_t at = 0;
    for (StringHeader* name : ordered) {
        Rooted<Value> key{rtCopyKeyToHeap(name)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
    }
    return out.get().rawBits();
}

// 20.1.2.11 Object.getOwnPropertySymbols: the SYMBOL half of OwnPropertyKeys,
// which 6.1.7.1 orders after every string key and among themselves in
// property-creation order. Non-enumerable symbol keys are included — this is
// getOwnPropertyKeys and not an enumeration, which is the same reason
// `getOwnPropertyNames` passes `enumerableOnly=false`.
//
// The order is a fact about the object's transition chain and nothing else. No
// hash table is consulted on the way here, and in particular the `Symbol.for`
// registry is not: a registered symbol has no privileged position among an
// object's keys.
uint64_t objectGetOwnPropertySymbols(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!args[0].isObject()) {
        return rtThrowTypeError(
                   "Object.getOwnPropertySymbols called on a value that is not an object")
            .rawBits();
    }
    // Where the receiver keeps symbol keys, which is `symbolKeyHolder`'s
    // question in rt_prop.cpp asked from the other side: a plain object keeps
    // them itself and a function keeps them in the side object its statics use.
    //
    // An array, a Map, a Set, a typed array and a RegExp carry no shape, so
    // they have nowhere to put one — and a symbol-keyed WRITE to any of them is
    // a named hard error, so the empty answer here is COMPLETE rather than a
    // gap dressed up as a result. That is the whole difference from
    // `getOwnPropertyNames`, which refuses those receivers: an array really has
    // an own `length`, so the string answer would be short by one.
    Rooted<Value> self{args[0]};
    ObjectHeader* holder = nullptr;
    HeapObjectHeader* hdr = self.get().asObject<HeapObjectHeader>();
    if (hdr->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        holder = reinterpret_cast<ObjectHeader*>(hdr);
    } else if (hdr->flags == 2) {
        Value props = self.get().asObject<FunctionHeader>()->properties;
        if (props.isObject()) holder = props.asObject<ObjectHeader>();
    }
    if (!holder) return bronze_create_array(0);

    const std::vector<PropertyKey> ordered =
        rtOwnKeysOrdered(holder, /*enumerableOnly=*/false);
    Rooted<Value> out{Value(bronze_create_array(0))};
    uint32_t at = 0;
    for (PropertyKey key : ordered) {
        if (!key.isSymbol()) continue;
        // A symbol lives in the arena and never moves, so it goes into the
        // result as it is — there is no `rtCopyKeyToHeap` counterpart, and a
        // copy would be a different symbol.
        Rooted<Value> sym{key.toValue()};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, sym);
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

// ---- Object.prototype -------------------------------------------------------
//
// The intrinsic every plain object inherits from. It is a real object on the
// real chain, found by the ordinary prototype walk — not a table consulted
// beside it, which is what every other builtin receiver in bronze still is. The
// difference is the whole point: a method here can be held, compared, passed to
// `.call`, and replaced, and `Object.getPrototypeOf({})` has something true to
// answer.
//
// Every member is defined NON-ENUMERABLE, per 20.1.3. That is not tidiness:
// `for-in` walks the prototype chain, so an enumerable member here would appear
// in every for-in over every object in the program. `Object.keys`, spread and
// `JSON.stringify` ask for own enumerable keys and so cannot see it either,
// which is why this object can be introduced under a suite of pinned
// expectations without moving one of them.

// The receiver of an Object.prototype method, which reaches one only through an
// ordinary call on a plain object or through `.call`. Three answers, and the
// third is the house rule: a receiver bronze cannot answer for is refused BY
// NAME rather than told that it has no own properties.
bool requireProtoReceiver(Value self, const char* method) {
    if (self.isNull() || self.isUndefined()) {
        rtThrowTypeError(std::string("Object.prototype.") + method +
                         " called on null or undefined");
        return false;
    }
    if (isPlainObject(self)) return true;
    fatal((std::string("unsupported: Object.prototype.") + method +
           " on a receiver that is not a plain object (an array, a function, a Map or a "
           "primitive reaches its members through the property path rather than through a "
           "prototype object, so bronze has no chain here to answer about)")
              .c_str());
}

uint64_t objectProtoHasOwnProperty(uint64_t, uint64_t thisBits, uint32_t argc,
                                   const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireProtoReceiver(self.get(), "hasOwnProperty")) {
        return Value::fromUndefined().rawBits();
    }
    return Value::fromBool(hasOwnPropertyNamed(self, args[0])).rawBits();
}

// 20.1.3.4. Own AND enumerable — a name that is only inherited answers false
// here where `in` answers true, and a class method (15.7.14 defines it
// non-enumerable) answers false where `hasOwnProperty` answers true.
uint64_t objectProtoPropertyIsEnumerable(uint64_t, uint64_t thisBits, uint32_t argc,
                                         const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireProtoReceiver(self.get(), "propertyIsEnumerable")) {
        return Value::fromUndefined().rawBits();
    }
    rtCheckStringExoticOwnKeys(self.get(), "testing");
    PropertyKey name = internOf(args[0]);
    auto* obj = self.get().asObject<ObjectHeader>();
    PropertyInfo info;
    if (!obj->shape || !obj->shape->lookupProperty(name, info)) {
        return Value::fromBool(false).rawBits();
    }
    return Value::fromBool(info.enumerable).rawBits();
}

// 20.1.3.3. Walks the ARGUMENT's chain looking for the receiver, so it answers
// about ancestry rather than about identity: an object is not its own
// prototype, and the walk starts one link up for that reason.
uint64_t objectProtoIsPrototypeOf(uint64_t, uint64_t thisBits, uint32_t argc,
                                  const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireProtoReceiver(self.get(), "isPrototypeOf")) {
        return Value::fromUndefined().rawBits();
    }
    if (!isPlainObject(args[0])) return Value::fromBool(false).rawBits();
    // No allocation in the loop, so the raw pointers stay valid throughout.
    ObjectHeader* walker = args[0].asObject<ObjectHeader>();
    ObjectHeader* target = self.get().asObject<ObjectHeader>();
    for (uint32_t depth = 0; depth < ObjectHeader::kMaxPrototypeDepth; ++depth) {
        walker = walker->protoAncestor(1);
        if (!walker) return Value::fromBool(false).rawBits();
        if (walker == target) return Value::fromBool(true).rawBits();
    }
    fatal("prototype chain too deep (a cycle?)");
}

// 20.1.3.7 ToObject(this), which for an object is the object. It exists so that
// the name is not a hole in the chain; it is NOT what makes `{} + 1` work,
// because ToPrimitive is what calls valueOf and ToPrimitive is still unbuilt
// (rt_convert.cpp names it).
uint64_t objectProtoValueOf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!requireProtoReceiver(self, "valueOf")) return Value::fromUndefined().rawBits();
    return self.rawBits();
}

const NamespaceFn kObjectProtoFunctions[] = {
    {"hasOwnProperty", objectProtoHasOwnProperty, 1},
    {"isPrototypeOf", objectProtoIsPrototypeOf, 1},
    {"propertyIsEnumerable", objectProtoPropertyIsEnumerable, 1},
    {"valueOf", objectProtoValueOf, 0},
};

// 20.1.3 members bronze has not built, diagnosed by name on a plain object's
// full-chain miss.
//
// `toString` is deliberately here rather than answered with "[object Object]".
// 20.1.3.6 is a tag lookup — Array, Function, Error, Arguments, each of the
// wrapper kinds — and bronze cannot ask the question for all of them: an error
// object here is an ordinary plain object with no [[ErrorData]] to find, so a
// toString written today would answer "[object Object]" for one and be
// confidently wrong at exactly the place `Object.prototype.toString.call(x)` is
// used. `toLocaleString` is 20.1.3.5, which calls toString.
const char* const kObjectProtoUnimplemented[] = {
    "toLocaleString",
    "toString",
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
    {"getOwnPropertyDescriptors", objectGetOwnPropertyDescriptors, 1},
    {"hasOwn", objectHasOwn, 2},
    {"is", objectIs, 2},
    {"getOwnPropertySymbols", objectGetOwnPropertySymbols, 1},
};

// Real members of `Object` that bronze has not built.
const char* const kObjectUnimplemented[] = {
    "groupBy",
};

Value g_objectNamespace = Value::fromUndefined();
Value g_objectPrototype = Value::fromUndefined();

// Both intrinsics, built together.
//
// They reference each other — `Object.prototype` is a property of the namespace
// and `Object.prototype.constructor` is the namespace — so neither can be built
// by an accessor that lazily builds the other: whichever ran first would
// re-enter the second, which would re-enter the first, and the recursion guard
// would hand out a half-built object. Building both here and publishing them at
// the end makes the cycle a pair of ordinary assignments.
//
// `constructor` matters more than it looks. Without it `({}).constructor` is
// `undefined`, which is a silent wrong answer, and one that would appear or
// disappear depending on whether the program had mentioned `Object` anywhere.
void ensureObjectIntrinsics() {
    if (g_objectPrototype.isObject()) return;

    // Object.prototype's own prototype is NULL, and it is the one object in the
    // program for which that is true by definition rather than by request
    // (20.1.3: "the value of [[Prototype]] is null"). It must not come from
    // `rtPlainObjectShape`, which is about to name THIS object as its
    // prototype — that would be the chain closing on itself.
    Rooted<Value> proto{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(Value::fromNull())))};
    proto.get().asObject<ObjectHeader>()->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;

    // Its own root shape, for the reason `Math` has one: a site reading
    // `Object.keys` must not share a transition tree with `{}` literals.
    Rooted<Value> ns{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(Value::fromUndefined())))};
    ns.get().asObject<ObjectHeader>()->header.flags = 0;

    for (const NamespaceFn& fn : kObjectFunctions) {
        Rooted<Value> key{rtMakeString(fn.name)};
        Rooted<Value> val{Value(bronze_function_singleton(fn.code, fn.arity))};
        ns.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }
    // Non-enumerable, and `defineOwn` because this is DefineOwnProperty and not
    // an assignment — 20.1.3 defines every one of these with
    // `enumerable: false`, and an enumerable member here would surface in every
    // `for-in` in the program, which walks the prototype chain.
    for (const NamespaceFn& fn : kObjectProtoFunctions) {
        Rooted<Value> key{rtMakeString(fn.name)};
        Rooted<Value> val{Value(bronze_function_singleton(fn.code, fn.arity))};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }

    // The two cross-references, on the same non-enumerable terms (20.1.2.1
    // makes `Object.prototype` non-writable and non-enumerable; 20.1.3.1 makes
    // `constructor` non-enumerable).
    {
        Rooted<Value> key{rtMakeString("prototype")};
        ns.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, proto, nullptr,
                                                  /*enumerable=*/false, /*defineOwn=*/true);
    }
    {
        Rooted<Value> key{rtMakeString("constructor")};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ns, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }

    g_objectNamespace = ns.get();
    g_objectPrototype = proto.get();
    rtHeap().add_permanent_root(&g_objectNamespace);
    rtHeap().add_permanent_root(&g_objectPrototype);
}

}  // namespace

Value rtObjectNamespace() {
    ensureObjectIntrinsics();
    return g_objectNamespace;
}

Value rtObjectPrototype() {
    ensureObjectIntrinsics();
    return g_objectPrototype;
}

void rtObjectProtoCheckMissingMember(const std::string& key) {
    rtCheckUnimplementedMember("Object.prototype", kObjectProtoUnimplemented,
                               std::size(kObjectProtoUnimplemented), key);
}

void rtObjectCheckMissingMember(Value obj, const std::string& key) {
    if (!g_objectNamespace.isObject() || obj.rawBits() != g_objectNamespace.rawBits()) return;
    rtCheckUnimplementedMember("Object", kObjectUnimplemented, std::size(kObjectUnimplemented),
                               key);
}

}  // namespace bronze::runtime
