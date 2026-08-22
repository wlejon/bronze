// getenv, as heap.cpp: the CRT-deprecation opt-out, not a blanket C4996
#define _CRT_SECURE_NO_WARNINGS

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
// The write side keeps an object in SHAPE-land whenever the descriptor can be
// represented there — a shape carries all four attributes, and transitions
// match on the full tuple, so `Object.defineProperty` in a hot constructor
// (three.js Object3D does exactly this) no longer costs every later property
// access its inline cache. Dictionary mode remains the escape for what a
// shared shape cannot express: redescribing attributes on one object of many,
// and the delete-shaped history it has always owned.
#include "runtime/builtin_object.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/dictionary.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/integrity.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

static bool shapeDefineEnabled() {
    static const bool enabled = []() {
        const char* env = std::getenv("BRONZE_NO_SHAPE_DEFINE");
        return !(env && std::strcmp(env, "1") == 0);
    }();
    return enabled;
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

// The attributes a DICTIONARY object's own property carries, in the same shape
// `Shape::lookupProperty` answers in. The two roads to "what is there now" are
// asked the same question by the descriptor defaults below, so they answer in
// the same type rather than in two.
bool dictAttributes(Value objVal, PropertyKey name, PropertyInfo& out) {
    const DictEntry* entry = entryOf(objVal, name);
    if (entry == nullptr) return false;
    out.slot = entry->slot;
    out.enumerable = entry->enumerable;
    out.accessor = entry->accessor;
    out.writable = entry->writable;
    out.configurable = entry->configurable;
    return true;
}

}  // namespace

// ECMA-262 10.1.6.3 DefineOwnProperty, for the one caller that can express a
// full descriptor. Plain data and accessor properties on shape-chain objects
// extend their shape transition tree rather than unconditionally degrading
// to dictionary mode.
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
    const bool wantWritable = hasWritable && bronze_truthy(writableV.get().rawBits());
    const bool wantEnumerable = hasEnumerable && bronze_truthy(enumerableV.get().rawBits());
    const bool wantConfigurable = hasConfigurable && bronze_truthy(configurableV.get().rawBits());

    // The key is built before the object is disturbed, and interned so the
    // entry can hold it forever.
    PropertyKey name = rtInternPropertyKey(args[1]);

    auto* obj = target.get().asObject<ObjectHeader>();

    // 10.1.6.3 step 4: "For each field of Desc, set the corresponding attribute
    // of the property named P to the value of the field." A field the
    // descriptor does NOT mention is left alone on an EXISTING property, and
    // only defaults to false on a new one (step 3, through
    // CompletePropertyDescriptor). Reading the three defaults as false either
    // way turned `Object.defineProperty(o, 'x', { value: 42 })` on a live
    // property into a silent freeze — non-writable, non-enumerable and
    // non-configurable — which is how three.js's `Object3D` gives itself an
    // `id` and how any code updates one value through a descriptor.
    PropertyInfo current;
    const bool present =
        obj->shape != nullptr &&
        (obj->shape->isDictionary() ? dictAttributes(target.get(), name, current)
                                    : obj->shape->lookupProperty(name, current));
    const bool writable = hasWritable ? wantWritable : (present && current.writable);
    const bool enumerable = hasEnumerable ? wantEnumerable : (present && current.enumerable);
    const bool configurable =
        hasConfigurable ? wantConfigurable : (present && current.configurable);
    if (shapeDefineEnabled() && obj->shape && !obj->shape->isDictionary()) {
        PropertyInfo existing;
        bool hasExisting = obj->shape->lookupProperty(name, existing);
        if (!hasExisting) {
            Rooted<Value> keyRoot{name.toValue()};
            if (accessor) {
                ObjectHeader::defineAccessor(rtHeap(), rtArena(), target, keyRoot, getter, setter,
                                             enumerable, configurable);
                return self.get().rawBits();
            } else {
                SetRefusal refusal = SetRefusal::None;
                target.get().asObject<ObjectHeader>()->setProp(
                    rtHeap(), rtArena(), keyRoot, value, /*ic=*/nullptr, enumerable,
                    /*defineOwn=*/true, /*receiver=*/nullptr, &refusal, writable, configurable);
                if (refusal == SetRefusal::NotExtensible) {
                    return rtThrowTypeError("Cannot define property, object is not extensible").rawBits();
                }
                return self.get().rawBits();
            }
        } else {
            if (!existing.configurable) {
                if (existing.accessor != accessor) {
                    return rtThrowTypeError("Cannot redefine property: " + keyText(name)).rawBits();
                }
                if (!existing.accessor) {
                    if (!existing.writable && (hasWritable && writable)) {
                        return rtThrowTypeError("Cannot redefine property: " + keyText(name)).rawBits();
                    }
                    if (!existing.writable && hasValue &&
                        obj->getSlot(existing.slot).rawBits() != value.get().rawBits()) {
                        return rtThrowTypeError("Cannot redefine property: " + keyText(name)).rawBits();
                    }
                    if (hasEnumerable && existing.enumerable != enumerable) {
                        return rtThrowTypeError("Cannot redefine property: " + keyText(name)).rawBits();
                    }
                    if (hasConfigurable && existing.configurable != configurable) {
                        return rtThrowTypeError("Cannot redefine property: " + keyText(name)).rawBits();
                    }
                    if (existing.writable && hasValue) {
                        obj->setSlot(existing.slot, value.get());
                    }
                    if (existing.writable && hasWritable && !writable) {
                        // The one attribute change 10.1.6.3 still permits when
                        // configurable is false: writable true -> false. A
                        // Shape is shared by every object that reached it, so
                        // the demotion cannot be written into the shape — this
                        // object diverges into dictionary mode instead, the
                        // same escape `delete` takes.
                        ObjectHeader::toDictionary(rtArena(), target);
                        entryOf(target.get(), name)->writable = false;
                    }
                    return self.get().rawBits();
                }
            } else if (existing.accessor == accessor && existing.enumerable == enumerable &&
                       existing.writable == writable && existing.configurable == configurable) {
                Rooted<Value> keyRoot{name.toValue()};
                if (accessor) {
                    ObjectHeader::defineAccessor(rtHeap(), rtArena(), target, keyRoot, getter, setter,
                                                 enumerable, configurable);
                } else if (hasValue) {
                    obj->setSlot(existing.slot, value.get());
                }
                return self.get().rawBits();
            }
        }
    }

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
            // The three own properties that live in the HEADER rather than in
            // the statics object, and so are invisible to the forward below:
            // `prototype` (10.2.4) and the `length`/`name` pair (10.2.10,
            // 10.2.9). Answered first, because the statics object is where a
            // `static` of the same name would be and there can be none — the
            // write path refuses all three.
            if (!args[1].isSymbol()) {
                const std::string key = rtObjectKeyTextOf(args[1]);
                if (rtExceptionPending()) return Value::fromUndefined().rawBits();
                const bool isProto = key == "prototype";
                const bool isPair = (key == "length" || key == "name") &&
                                    args[0].asObject<FunctionHeader>()->name != nullptr;
                if (isProto || isPair) {
                    Rooted<Value> self{args[0]};
                    Rooted<Value> value{Value::fromUndefined()};
                    if (isProto) {
                        rtEnsureFunctionPrototype(self);
                        value.set(self.get().asObject<FunctionHeader>()->prototype);
                    } else if (key == "length") {
                        value.set(
                            Value::fromDouble(self.get().asObject<FunctionHeader>()->length));
                    } else {
                        value.set(rtKeyAsValue(self.get().asObject<FunctionHeader>()->name));
                    }
                    // 10.2.4 makes `prototype` non-enumerable and
                    // non-configurable, writable unless the function is one
                    // whose prototype never was; 10.2.9 and 10.2.10 make the
                    // pair non-writable, non-enumerable and CONFIGURABLE.
                    const bool writable =
                        isProto && rtFunctionPrototypeWritable(self.get());
                    const bool configurable = !isProto;
                    Rooted<Value> out{Value(bronze_create_object())};
                    putField(out, "value", value);
                    Rooted<Value> w{Value::fromBool(writable)};
                    putField(out, "writable", w);
                    Rooted<Value> e{Value::fromBool(false)};
                    putField(out, "enumerable", e);
                    Rooted<Value> c{Value::fromBool(configurable)};
                    putField(out, "configurable", c);
                    return out.get().rawBits();
                }
            }
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
