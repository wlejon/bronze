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
//
// Every function here takes its subject as an ARGUMENT. The ones that take it
// as `this` are `Object.prototype`'s and live in builtin_object_proto.cpp,
// which is the seam: a static raises the TypeError its clause names for a
// receiver it will not take, while a method can only have been REACHED through
// its receiver, so a kind bronze cannot walk is a refusal there rather than a
// throw. `ensureObjectIntrinsics` at the foot of this file still builds both
// objects, because each is a property of the other.

#include <iterator>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/dictionary.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/integrity.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

// ToPropertyKey (7.1.19) into the immortal form a DictEntry can hold. Interning
// is not an optimization here: a DictEntry outlives every collection, so a heap
// key would dangle.
//
// A SYMBOL is returned as it stands and never converted: it already lives in
// the arena, and running ToString on one is the TypeError this whole type
// exists to raise. That is also what lets `Object.defineProperty`,
// `getOwnPropertyDescriptor` and `hasOwn` take a symbol key without a branch of
// their own.
PropertyKey rtInternPropertyKey(Value keyVal) {
    if (keyVal.isSymbol()) return PropertyKey::fromValue(keyVal);
    Rooted<Value> str{rtValueToString(keyVal)};
    return PropertyKey::forString(
        StringHeader::internToArena(rtArena(), str.get().asString<StringHeader>()));
}

// Own-property existence, shared by `Object.prototype.hasOwnProperty` and
// `Object.hasOwn` — 20.1.3.2 and 20.1.2.13 are the same operation with the
// receiver in a different position, and writing it twice is how the two would
// come to disagree about a dictionary-mode object.
//
// The key is interned BEFORE the object is read: `rtInternPropertyKey` runs
// ToString, which allocates, so an ObjectHeader* taken across it would be stale.
bool rtHasOwnPropertyNamed(Rooted<Value>& self, Value key) {
    // A String exotic object's own keys are not all in its shape: 10.4.3.4
    // synthesises index properties from the wrapped characters, and bronze
    // answers those on the property path only. Refused by name rather than
    // reported absent (rt_object.cpp says why they are not materialised).
    rtCheckStringExoticOwnKeys(self.get(), "testing");
    PropertyKey name = rtInternPropertyKey(key);
    auto* obj = self.get().asObject<ObjectHeader>();
    uint32_t slot = 0;
    return obj->shape && obj->shape->lookupProperty(name, slot);
}

namespace {

bool isPlainObject(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN;
}

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// Why bronze cannot describe or redefine this receiver's own properties. One
// sentence per storage story, because "unsupported" without the reason is a
// reader's dead end.
const char* propertyStoreReason(Value v) {
    switch (v.asObject<HeapObjectHeader>()->flags) {
        case HeapKind::Array:
            return "its own keys are ELEMENTS and a `length`, and neither is a property "
                   "bronze keeps in a shape a descriptor could be written to";
        case HeapKind::Function:
            return "its own keys are a `prototype` slot, a side object of statics, and "
                   "`length` and `name`, which bronze does not store at all";
        default:
            return "it keeps no property table, so there is nothing here to describe";
    }
}

// The receiver of an `Object` member that needs a property TABLE — one it can
// describe, redefine, or copy into.
//
// `isPlainObject` is the wrong predicate to gate these on, and the wrongness is
// not a matter of degree. It answers "does this keep its properties in a
// shape"; the first step of every clause below asks whether the value is an
// OBJECT, and an array is an object. So each of these told a program its array
// "is not an object" — a false statement about the receiver, and the kind that
// sends a reader looking for the wrong bug. It is the same mistake the
// integrity levels made: a predicate answering a different question than the
// step asks.
//
// Three answers, and the middle one is the fix. A plain object proceeds. A
// PRIMITIVE gets the TypeError the clause specifies, and the message is now
// true of what it was actually given. Any other object is refused BY NAME,
// saying what it is and what about its storage bronze cannot reach —
// `getOwnPropertyNames`'s precedent, which named the kinds before any of the
// rest of them did.
[[noreturn]] void refuseObjectKind(Value v, const char* member) {
    fatal((std::string("unsupported: Object.") + member + " on " + rtObjectKindName(v) + " (" +
           propertyStoreReason(v) + ")")
              .c_str());
}

bool requirePropertyTable(Value v, const char* member) {
    if (isPlainObject(v)) return true;
    if (!v.isObject()) {
        rtThrowTypeError(std::string("Object.") + member +
                         " called on a value that is not an object");
        return false;
    }
    refuseObjectKind(v, member);
}

// Where a receiver's OWN KEYS come from — which is what the members whose step
// 1 is ToObject actually need, and it is less than the object itself.
//
// The box is deliberately not built, the arrangement `Object.getPrototypeOf`
// takes for a primitive. Here it buys more than a skipped allocation: a Number
// object and a Symbol object are boxes bronze cannot make at all, having no
// `Number.prototype` or `Symbol.prototype` to point them at — and neither has
// an own property, so `None` is the COMPLETE answer rather than the one bronze
// can reach. `requirePropertyTable` above stays the gate for `defineProperty`
// and `defineProperties`, whose step 1 is "If O is not an Object, throw"
// (20.1.2.4, 20.1.2.3) and not ToObject.
enum class OwnKeys {
    Shape,        // a plain object: its own keys are in its shape
    StringChars,  // a primitive string: 10.4.3 synthesises them from the characters
    None,         // a number, a boolean, a symbol: the box has no own property
    Threw,        // null or undefined: ToObject has no answer, and this raised it
};

OwnKeys ownKeysOf(Value v, const char* member) {
    if (v.isNull() || v.isUndefined()) {
        // The same sentence the other refusals use, and it is true of exactly
        // these two: 7.1.18's only failures are the two values that are not
        // objects and have no box.
        rtThrowTypeError(std::string("Object.") + member +
                         " called on a value that is not an object");
        return OwnKeys::Threw;
    }
    if (v.isString()) return OwnKeys::StringChars;
    if (!v.isObject()) return OwnKeys::None;
    if (isPlainObject(v)) return OwnKeys::Shape;
    refuseObjectKind(v, member);
}

// ToPropertyKey (7.1.19) as the text an own-key question compares. A SYMBOL is
// never one of the keys a string or an empty box has, so the caller answers for
// one without converting it — which is also the only way to answer, since
// ToString of a symbol is a TypeError.
std::string keyTextOf(Value keyVal) {
    Rooted<Value> str{rtValueToString(keyVal)};
    if (rtExceptionPending()) return std::string();
    return rtUtf8Chars(str.get().asString<StringHeader>());
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
    if (!requirePropertyTable(args[0], "defineProperty")) return Value::fromUndefined().rawBits();
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
    PropertyKey name = rtInternPropertyKey(args[1]);

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
    switch (ownKeysOf(args[0], "getOwnPropertyDescriptor")) {
        case OwnKeys::Threw:
            return Value::fromUndefined().rawBits();
        case OwnKeys::None:
            // The box has no own property, so every key misses — which is
            // `undefined`, the same answer a plain object gives for a name it
            // does not carry.
            return Value::fromUndefined().rawBits();
        case OwnKeys::StringChars: {
            if (args[1].isSymbol()) return Value::fromUndefined().rawBits();
            const std::string key = keyTextOf(args[1]);
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
            StringOwnProperty own;
            if (!rtStringDataOwnProperty(args[0], key, own)) {
                return Value::fromUndefined().rawBits();
            }
            // 6.2.6.4 FromPropertyDescriptor in the same field order as below,
            // over the attributes 10.4.3 fixes: non-writable and
            // non-configurable for both kinds of own key, and enumerable for an
            // index alone. Rooted first — building the result allocates.
            Rooted<Value> value{own.value};
            Rooted<Value> out{Value(bronze_create_object())};
            putField(out, "value", value);
            Rooted<Value> w{Value::fromBool(false)};
            putField(out, "writable", w);
            Rooted<Value> e{Value::fromBool(own.enumerable)};
            putField(out, "enumerable", e);
            Rooted<Value> c{Value::fromBool(false)};
            putField(out, "configurable", c);
            return out.get().rawBits();
        }
        case OwnKeys::Shape:
            break;
    }
    Rooted<Value> self{args[0]};
    rtCheckStringExoticOwnKeys(self.get(), "describing");
    PropertyKey name = rtInternPropertyKey(args[1]);

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

// The six integrity-level members — `freeze`, `seal`, `preventExtensions` and
// their predicates — are in integrity.cpp. They left this file when they
// stopped being about plain objects: what each of them DOES is decide where the
// receiver keeps [[Extensible]], and that question belongs beside every path
// that reads the answer back (an array's element write, a function's
// `prototype`), not beside `Object.keys`.

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
//
// Step 1 of 20.1.2.12 is ToObject, which is the whole reason a PRIMITIVE has an
// answer here at all: `Object.getPrototypeOf("x")` is String.prototype, and
// only `null` and `undefined` are the TypeError (7.1.18). The wrapper ToObject
// would build is not built — [[GetPrototypeOf]] of one is the intrinsic
// whatever it wraps, so building it would allocate an object to read a constant
// off it.
uint64_t objectGetPrototypeOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) {
        if (args[0].isNull() || args[0].isUndefined()) {
            return rtThrowTypeError("Object.getPrototypeOf called on " +
                                    std::string(args[0].isNull() ? "null" : "undefined"))
                .rawBits();
        }
        if (args[0].isString()) return rtStringPrototype().rawBits();
        if (args[0].isBool()) return rtBooleanPrototype().rawBits();
        if (!args[0].isObject()) {
            // A number's and a symbol's members are still handed out beside the
            // value, so there is no object to return and no honest way to
            // invent one: `null` would deny a chain that really does carry
            // `toFixed`, and `Object.prototype` would name the wrong holder.
            fatal("unsupported: Object.getPrototypeOf of a number or a symbol needs "
                  "Number.prototype / Symbol.prototype, which bronze does not provide");
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
    switch (ownKeysOf(args[0], "hasOwn")) {
        case OwnKeys::Threw:
            return Value::fromUndefined().rawBits();
        case OwnKeys::None:
            return Value::fromBool(false).rawBits();
        case OwnKeys::StringChars: {
            // A symbol is never an own key of a String exotic object: 10.4.3.3
            // reports the indices and `length`, all of them strings.
            if (args[1].isSymbol()) return Value::fromBool(false).rawBits();
            const std::string key = keyTextOf(args[1]);
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
            // args[0] re-read through RootedArgs: `keyTextOf` allocates.
            return Value::fromBool(rtStringDataHasOwnKey(args[0], key)).rawBits();
        }
        case OwnKeys::Shape:
            break;
    }
    Rooted<Value> self{args[0]};
    return Value::fromBool(rtHasOwnPropertyNamed(self, args[1])).rawBits();
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
    // The receiver classification happens here only to raise (or refuse) before
    // any work; every source below is then handled by the two members this is
    // defined in terms of, which is the point of defining it in terms of them.
    if (ownKeysOf(args[0], "getOwnPropertyDescriptors") == OwnKeys::Threw) {
        return Value::fromUndefined().rawBits();
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
    if (!requirePropertyTable(args[0], "defineProperties")) {
        return Value::fromUndefined().rawBits();
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
    // An array's own names include `length`, which bronze stores outside the
    // shape system entirely, so the answer would be incomplete rather than
    // merely different — which is what the shared refusal says, by kind.
    switch (ownKeysOf(args[0], "getOwnPropertyNames")) {
        case OwnKeys::Threw:
            return Value::fromUndefined().rawBits();
        case OwnKeys::None:
            return bronze_create_array(0);
        case OwnKeys::StringChars:
            // 10.4.3.3's own order — the indices ascending and THEN `length` —
            // and `length` is here where `Object.keys` drops it, because this
            // member is OwnPropertyKeys without the enumerable filter.
            return rtStringOwnKeyNames(args[0], /*enumerableOnly=*/false).rawBits();
        case OwnKeys::Shape:
            break;
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
    // An array, a Map, a Set, a typed array and a RegExp answer none. The
    // reason is the WRITE side and not the storage: a symbol-keyed write to any
    // of them is a named hard error (rt_prop.cpp's `symbolKeyHolder` admits
    // only a plain object and a function), so no symbol can ever have reached
    // one and the empty answer is COMPLETE rather than a gap dressed up as a
    // result. An array's side object of named properties is deliberately not
    // consulted: nothing can put a symbol there. That is the whole difference
    // from
    // `getOwnPropertyNames`, which refuses those receivers: an array really has
    // an own `length`, so the string answer would be short by one.
    Rooted<Value> self{args[0]};
    ObjectHeader* holder = nullptr;
    HeapObjectHeader* hdr = self.get().asObject<HeapObjectHeader>();
    if (hdr->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        holder = reinterpret_cast<ObjectHeader*>(hdr);
    } else if (hdr->flags == HeapKind::Function) {
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
// 20.1.2.1 step 1's ToObject, and the one place in this file where the box has
// to be BUILT rather than reasoned about: it is the value `assign` returns, so
// the program can hold it. The two bronze has — the String and Boolean exotic
// objects — are built; a number and a symbol are refused by name, which is the
// same missing intrinsic `Object.getPrototypeOf` of a number names.
Value toObjectForAssign(Value v) {
    if (v.isNull() || v.isUndefined()) {
        rtThrowTypeError("Object.assign called on a value that is not an object");
        return Value::fromUndefined();
    }
    if (v.isString()) {
        Rooted<Value> str{v};
        return rtMakeStringWrapper(str);
    }
    if (v.isBool()) return rtMakeBooleanWrapper(v.asBool());
    if (!v.isObject()) {
        fatal("unsupported: Object.assign with a number or a symbol as the target (7.1.18 "
              "boxes it, and bronze has no Number.prototype / Symbol.prototype for the box "
              "to be an instance of)");
    }
    if (isPlainObject(v)) return v;
    refuseObjectKind(v, "assign");
}

uint64_t objectAssign(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // The TARGET is the only argument with a kind requirement — a source that
    // is not an object contributes no properties rather than raising.
    Rooted<Value> target{toObjectForAssign(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
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
        bronze_elem_set(out.get().rawBits(), k.get().rawBits(), v.get().rawBits(), /*strict=*/false);
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
    {"freeze", rtObjectFreeze, 1},
    {"isFrozen", rtObjectIsFrozen, 1},
    {"seal", rtObjectSeal, 1},
    {"isSealed", rtObjectIsSealed, 1},
    {"preventExtensions", rtObjectPreventExtensions, 1},
    {"isExtensible", rtObjectIsExtensible, 1},
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
    ns.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;

    for (const NamespaceFn& fn : kObjectFunctions) {
        Rooted<Value> key{rtMakeString(fn.name)};
        Rooted<Value> val{Value(bronze_function_singleton(fn.code, fn.arity))};
        ns.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }
    // The prototype's own members come from builtin_object_proto.cpp, which owns
    // every function whose subject is `this` rather than an argument. They are
    // installed from here because 20.1.2.1 and 20.1.3.1 make these two objects
    // each other's property, so one initializer has to hold both.
    rtInstallObjectProtoMethods(proto);

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

void rtObjectCheckMissingMember(Value obj, const std::string& key) {
    if (!g_objectNamespace.isObject() || obj.rawBits() != g_objectNamespace.rawBits()) return;
    rtCheckUnimplementedMember("Object", kObjectUnimplemented, std::size(kObjectUnimplemented),
                               key);
}

}  // namespace bronze::runtime
