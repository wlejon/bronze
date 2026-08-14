// The property descriptor as a reified object: the four `Object` members that
// convert between it and bronze's internal form.
//
// 6.2.6.4 FromPropertyDescriptor turns what an object actually stores — a shape
// slot, and the `writable` and `configurable` bits a dictionary entry carries —
// into an ordinary object a program can hold; 6.2.6.5 ToPropertyDescriptor
// reads one back. `getOwnPropertyDescriptor` and `getOwnPropertyDescriptors`
// are the first direction, `defineProperty` and `defineProperties` the second,
// and that round trip is what makes them one subject rather than four members
// that happened to be next to each other.
//
// The FIELD ORDER of the object built here is the specification's and not a
// convenience: `Object.keys(descriptor)` prints it, so it is pinned bytes.
//
// Everything here goes through DICTIONARY mode on the write side, including a
// descriptor that asks for nothing unusual — `writable` and `configurable` live
// in the dictionary entry and nowhere else. That is what keeps the inline
// caches out of this file entirely, and it is the reason the split is drawn
// here: builtin_object.cpp's members all read an object through its shape, and
// none of them moves it out of one.

#include "runtime/builtin_object.h"

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/dictionary.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

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

}  // namespace

// ECMA-262 10.1.6.3 DefineOwnProperty, for the one caller that can express a
// full descriptor. Every path goes through dictionary mode, including a
// descriptor that asks for nothing unusual: `{ value: 5 }` DEFAULTS its three
// missing attributes to false (6.2.6.5), so the plain-looking case is exactly
// the one a shape transition cannot represent.
uint64_t rtObjectDefineProperty(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!rtObjectRequirePropertyTable(args[0], "defineProperty")) {
        return Value::fromUndefined().rawBits();
    }
    if (!rtObjectIsPlain(args[2])) {
        return rtThrowTypeError("Property description must be an object").rawBits();
    }
    Rooted<Value> self{args[0]};
    Rooted<Value> desc{args[2]};

    Rooted<Value> target{self.get()};
    if (self.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        rtEnsureFunctionProperties(self);
        target.set(self.get().asObject<FunctionHeader>()->properties);
    }

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

    ObjectHeader::toDictionary(rtArena(), target);
    DictEntry* existing = entryOf(target.get(), name);
    if (!existing && !target.get().asObject<ObjectHeader>()->shape->dict->extensible) {
        return rtThrowTypeError("Cannot define property, object is not extensible").rawBits();
    }
    if (existing && !existing->configurable) {
        return rtThrowTypeError("Cannot redefine property: " + keyText(name)).rawBits();
    }

    uint32_t slot = 0;
    ObjectHeader* live =
        ObjectHeader::dictDefine(rtHeap(), rtArena(), target, name, enumerable, accessor, slot);
    if (accessor) {
        live->setSlot(slot, hasGet ? getter.get() : Value::fromUndefined());
        live->setSlot(slot + 1, hasSet ? setter.get() : Value::fromUndefined());
    } else {
        live->setSlot(slot, value.get());
    }
    DictEntry* entry = entryOf(target.get(), name);
    entry->writable = writable;
    entry->configurable = configurable;
    return self.get().rawBits();
}

// 6.2.6.4 FromPropertyDescriptor. The FIELD ORDER is the specification's, not
// a convenience — `Object.keys(descriptor)` prints it, so it is pinned bytes.
uint64_t rtObjectGetOwnPropertyDescriptor(uint64_t, uint64_t, uint32_t argc,
                                          const uint64_t* argv) {
    RootedArgs args(argc, argv);
    switch (rtObjectOwnKeysOf(args[0], "getOwnPropertyDescriptor")) {
        case ObjectOwnKeys::Threw:
            return Value::fromUndefined().rawBits();
        case ObjectOwnKeys::None:
            // The box has no own property, so every key misses — which is
            // `undefined`, the same answer a plain object gives for a name it
            // does not carry.
            return Value::fromUndefined().rawBits();
        case ObjectOwnKeys::StringChars: {
            if (args[1].isSymbol()) return Value::fromUndefined().rawBits();
            const std::string key = rtObjectKeyTextOf(args[1]);
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
            Value data = args[0];
            if (!data.isString()) rtStringWrapperData(args[0], data);
            StringOwnProperty own;
            if (!rtStringDataOwnProperty(data, key, own)) {
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
        case ObjectOwnKeys::Namespace: {
            // 10.4.6.1 gives a namespace one own SYMBOL-keyed property —
            // `@@toStringTag`, the string "Module" — and it is the one own key
            // of one that is not an export, so it is answered before the export
            // table is consulted.
            if (Value tag; rtModuleNamespaceOwnSymbol(args[0], args[1], tag)) {
                Rooted<Value> value{tag};
                Rooted<Value> out{Value(bronze_create_object())};
                putField(out, "value", value);
                // All three false: 10.4.6.1 defines it { [[Writable]]: false,
                // [[Enumerable]]: false, [[Configurable]]: false }.
                Rooted<Value> w{Value::fromBool(false)};
                putField(out, "writable", w);
                Rooted<Value> e{Value::fromBool(false)};
                putField(out, "enumerable", e);
                Rooted<Value> c{Value::fromBool(false)};
                putField(out, "configurable", c);
                return out.get().rawBits();
            }
            Value found;
            // False is 10.4.6.5's `undefined` — a name the module does not
            // export has no descriptor at all, which is not the same as a
            // descriptor of `undefined`.
            if (!rtModuleNamespaceOwnProperty(args[0], args[1], found)) {
                return Value::fromUndefined().rawBits();
            }
            Rooted<Value> value{found};
            Rooted<Value> out{Value(bronze_create_object())};
            putField(out, "value", value);
            // `writable: true` is 10.4.6.5's own answer and is not a slip: the
            // EXPORTING module may still assign to the binding, and 6.1.7.3
            // forbids a non-writable non-configurable property whose value
            // changes. What refuses `ns.x = 1` is [[Set]] (10.4.6.9), which
            // returns false whatever this descriptor says — the two are
            // different internal methods and only one of them is an attribute.
            Rooted<Value> w{Value::fromBool(true)};
            putField(out, "writable", w);
            Rooted<Value> e{Value::fromBool(true)};
            putField(out, "enumerable", e);
            Rooted<Value> c{Value::fromBool(false)};
            putField(out, "configurable", c);
            return out.get().rawBits();
        }
        case ObjectOwnKeys::Function: {
            Value props = args[0].asObject<FunctionHeader>()->properties;
            if (props.isUndefined() || !props.isObject()) {
                return Value::fromUndefined().rawBits();
            }
            const uint64_t call[2] = {props.rawBits(), args[1].rawBits()};
            return rtObjectGetOwnPropertyDescriptor(0, 0, 2, call);
        }
        case ObjectOwnKeys::Shape:
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

// 20.1.2.9 Object.getOwnPropertyDescriptors: one entry per own property, each
// the object `getOwnPropertyDescriptor` builds. Defined in terms of it (step 4
// calls it per key), so it calls it, rather than growing a second copy of the
// descriptor shape that could drift from the first.
uint64_t rtObjectGetOwnPropertyDescriptors(uint64_t, uint64_t, uint32_t argc,
                                           const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // The receiver classification happens here only to raise (or refuse) before
    // any work; every source below is then handled by the two members this is
    // defined in terms of, which is the point of defining it in terms of them.
    if (rtObjectOwnKeysOf(args[0], "getOwnPropertyDescriptors") == ObjectOwnKeys::Threw) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> self{args[0]};
    Rooted<Value> out{Value(bronze_create_object())};
    // ALL own keys, not just the enumerable ones (20.1.2.9 step 2 is
    // OwnPropertyKeys), which is where this differs from `Object.keys`.
    const uint64_t ownCall[1] = {self.get().rawBits()};
    Rooted<Value> names{Value(rtObjectGetOwnPropertyNames(0, 0, 1, ownCall))};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    const uint32_t count = names.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> key{names.get().asObject<ArrayHeader>()->getElem(i)};
        const uint64_t call[2] = {self.get().rawBits(), key.get().rawBits()};
        Rooted<Value> desc{Value(rtObjectGetOwnPropertyDescriptor(0, 0, 2, call))};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        putField(out, key, desc);
    }
    return out.get().rawBits();
}

// 20.1.2.3 Object.defineProperties, and the loop `Object.create`'s second
// argument shares with it.
bool rtObjectDefineFromDescriptors(Rooted<Value>& target, Rooted<Value>& descriptors) {
    if (!rtObjectIsPlain(descriptors.get())) {
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
        rtObjectDefineProperty(0, 0, 3, call);
        if (rtExceptionPending()) return false;
    }
    return true;
}

uint64_t rtObjectDefineProperties(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!rtObjectRequirePropertyTable(args[0], "defineProperties")) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> target{args[0]};
    Rooted<Value> descriptors{args[1]};
    rtObjectDefineFromDescriptors(target, descriptors);
    return target.get().rawBits();
}

}  // namespace bronze::runtime
