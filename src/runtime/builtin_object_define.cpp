// The WRITE side of the property descriptor: 10.1.6.3 [[DefineOwnProperty]]
// and 10.4.2.1's array variant over a descriptor builtin_object_descriptor.cpp
// has already decoded, and the two `Object` members and the one ABI entry
// point defined over it — `defineProperty`, `defineProperties`, and the
// compile-time-decoded `bronze_define_own_attr` the lowering emits for a
// literal batch.
//
// The apply keeps an object in SHAPE-land whenever the descriptor can be
// represented there — a shape carries all four attributes, and transitions
// match on the full tuple, so `Object.defineProperty` in a hot constructor
// (three.js Object3D does exactly this) no longer costs every later property
// access its inline cache. Dictionary mode remains the escape for what a
// shared shape cannot express: redescribing attributes on one object of many,
// and the delete-shaped history it has always owned.
//
// The RESULT of every apply is the boolean [[DefineOwnProperty]] answers, not
// a throw: the two members defined over it disagree about what a refusal means
// (20.1.2.4 raises, 28.1.3 returns false). `throwOnRefusal` chooses which of
// the two a call is; the errors of the DECODE are raised by the decode.
#include "runtime/builtin_object.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_object_descriptor_internal.h"
#include "runtime/dictionary.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/integrity.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

using namespace descriptor_internal;

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

// A [[DefineOwnProperty]] that REFUSED. 20.1.2.4 `Object.defineProperty` turns
// the refusal into a TypeError (step 4 is `DefinePropertyOrThrow`); 28.1.3
// `Reflect.defineProperty` returns the boolean instead and must not throw for
// it. So a refusal is a VALUE here and becomes a throw only at the entry point
// that asked for one.
bool refuseDefine(bool throwOnRefusal, const std::string& message) {
    if (throwOnRefusal) rtThrowTypeError(message);
    return false;
}

}  // namespace

// ECMA-262 10.1.6.3 DefineOwnProperty over a descriptor that is already
// decoded. Plain data and accessor properties on shape-chain objects extend
// their shape transition tree rather than unconditionally degrading to
// dictionary mode.
//
// The RESULT is the boolean [[DefineOwnProperty]] answers, not a throw: the two
// members defined over it disagree about what a refusal means (20.1.2.4 raises,
// 28.1.3 returns false). `throwOnRefusal` chooses which of the two this call
// is; the errors of the DECODE are raised by the decode, above this.
//
// `target` must already be the object that HOLDS the properties — a function's
// statics box rather than the function — and `name` must already be interned.
static bool applyDecodedDescriptor(Rooted<Value>& target, PropertyKey name,
                                   const DecodedDescriptor& d, Rooted<Value>& value,
                                   Rooted<Value>& getter, Rooted<Value>& setter,
                                   bool throwOnRefusal) {
    const bool hasValue = d.hasValue;
    const bool hasWritable = d.hasWritable;
    const bool hasEnumerable = d.hasEnumerable;
    const bool hasConfigurable = d.hasConfigurable;
    const bool hasGet = d.hasGet;
    const bool hasSet = d.hasSet;
    const bool wantWritable = d.wantWritable;
    const bool wantEnumerable = d.wantEnumerable;
    const bool wantConfigurable = d.wantConfigurable;
    const bool accessor = hasGet || hasSet;
    // 6.2.6.1 IsGenericDescriptor: a descriptor that names neither a data field
    // nor an accessor field. 10.1.6.3 lets one through every kind test — it
    // changes attributes and says nothing about what the property HOLDS — so it
    // must not be read as "an accessor descriptor with no accessors".
    const bool descGeneric = !hasValue && !hasWritable && !hasGet && !hasSet;

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
        // The same lookup the defaults above already made: this branch is
        // reached only when the shape is not a dictionary, which is exactly the
        // case `present` answered with `lookupProperty`. Asking twice is a
        // chain walk and a key compare per define, and a constructor that
        // defines six properties pays it six times.
        const bool hasExisting = present;
        const PropertyInfo& existing = current;
        if (!hasExisting) {
            Rooted<Value> keyRoot{name.toValue()};
            if (accessor) {
                ObjectHeader::defineAccessor(rtHeap(), rtArena(), target, keyRoot, getter, setter,
                                             enumerable, configurable);
                return true;
            } else {
                SetRefusal refusal = SetRefusal::None;
                target.get().asObject<ObjectHeader>()->setProp(
                    rtHeap(), rtArena(), keyRoot, value, /*ic=*/nullptr, enumerable,
                    /*defineOwn=*/true, /*receiver=*/nullptr, &refusal, writable, configurable);
                if (refusal == SetRefusal::NotExtensible) {
                    return refuseDefine(throwOnRefusal,
                                        "Cannot define property, object is not extensible");
                }
                return true;
            }
        } else {
            if (!existing.configurable) {
                // 10.1.6.3 step 4, over a property whose attributes a SHAPE
                // carries. A GENERIC descriptor passes the kind test (4.c) —
                // it says nothing about the kind — which is what lets
                // `{ enumerable: true }` describe an accessor without claiming
                // to be one.
                if (!descGeneric && existing.accessor != accessor) {
                    return refuseDefine(throwOnRefusal,
                                        "Cannot redefine property: " + keyText(name));
                }
                if (!existing.accessor) {
                    // 4.a and 4.b: the two attributes a non-configurable
                    // property may not be given, whatever kind it is.
                    if (hasConfigurable && configurable) {
                        return refuseDefine(throwOnRefusal,
                                            "Cannot redefine property: " + keyText(name));
                    }
                    if (hasEnumerable && existing.enumerable != enumerable) {
                        return refuseDefine(throwOnRefusal,
                                            "Cannot redefine property: " + keyText(name));
                    }
                    if (!existing.writable) {
                        // 4.e: a frozen property still accepts a redefinition
                        // that CHANGES NOTHING, which is why the value is
                        // compared rather than the presence of the field. The
                        // relation is 7.2.11 SameValue and not a bit compare:
                        // two strings with the same characters are the same
                        // value and two distinct heap strings are not the same
                        // pointer, so `defineProperty(frozen, k, {value: s})`
                        // refused a redefinition to the value already there.
                        if (hasWritable && writable) {
                            return refuseDefine(throwOnRefusal,
                                                "Cannot redefine property: " + keyText(name));
                        }
                        if (hasValue && !sameValue(obj->getSlot(existing.slot), value.get())) {
                            return refuseDefine(throwOnRefusal,
                                                "Cannot redefine property: " + keyText(name));
                        }
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
                    return true;
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
                return true;
            }
        }
    }

    ObjectHeader::toDictionary(rtArena(), target);
    DictEntry* existing = entryOf(target.get(), name);
    if (!existing) {
        if (!target.get().asObject<ObjectHeader>()->shape->dict->extensible) {
            return refuseDefine(throwOnRefusal, "Cannot define property, object is not extensible");
        }
    } else if (!existing->configurable) {
        // 10.1.6.3 step 4, the same five tests the shape branch above spells,
        // over the storage a dictionary uses. A non-configurable property is
        // not simply closed: it still accepts a redefinition that changes
        // NOTHING, and a writable one still accepts the demotion to
        // non-writable, so what decides is a comparison and not the presence
        // of the field. Refusing on presence alone is what made
        // `defineProperty(Object.freeze({x: 1}), 'x', {value: 1})` a TypeError.
        auto* live = target.get().asObject<ObjectHeader>();
        bool valid = true;
        if (hasConfigurable && configurable) {
            valid = false;  // 4.a
        } else if (hasEnumerable && existing->enumerable != enumerable) {
            valid = false;  // 4.b
        } else if (!descGeneric && existing->accessor != accessor) {
            valid = false;  // 4.c
        } else if (existing->accessor) {
            // 4.d, and SameValue on each half: the same accessor function
            // redefined onto itself is a define that changes nothing.
            valid = (!hasGet || sameValue(getter.get(), live->getSlot(existing->slot))) &&
                    (!hasSet || sameValue(setter.get(), live->getSlot(existing->slot + 1)));
        } else if (!existing->writable) {
            // 4.e, as above.
            valid = !(hasWritable && writable) &&
                    (!hasValue || sameValue(value.get(), live->getSlot(existing->slot)));
        }
        if (!valid) {
            return refuseDefine(throwOnRefusal, "Cannot redefine property: " + keyText(name));
        }
    }

    // 10.1.6.3 step 4: a GENERIC descriptor does not change the property's
    // KIND, so `{ enumerable: false }` on an accessor leaves an accessor.
    const bool resultAccessor = (descGeneric && existing) ? existing->accessor : accessor;
    // And step 5 sets only the fields the descriptor HAS. What it omits keeps
    // the value the property already holds — an absent `value` on a live data
    // property, an absent `get` on a live accessor — which is what separates a
    // partial redefinition from a replacement. Writing `value` unconditionally
    // is what turned `defineProperty(o, 'a', {writable: false})` into a store
    // of `undefined` over whatever `o.a` was.
    //
    // Read here, because `dictDefine` below can move the object and reallocate
    // the slots it is being read out of.
    Rooted<Value> keptValue{Value::fromUndefined()};
    Rooted<Value> keptSetter{Value::fromUndefined()};
    if (existing != nullptr && existing->accessor == resultAccessor) {
        auto* live = target.get().asObject<ObjectHeader>();
        keptValue.set(live->getSlot(existing->slot));
        if (resultAccessor) keptSetter.set(live->getSlot(existing->slot + 1));
    }

    uint32_t slot = 0;
    ObjectHeader* live = ObjectHeader::dictDefine(rtHeap(), rtArena(), target, name, enumerable,
                                                  resultAccessor, slot);
    if (resultAccessor) {
        live->setSlot(slot, hasGet ? getter.get() : keptValue.get());
        live->setSlot(slot + 1, hasSet ? setter.get() : keptSetter.get());
    } else {
        live->setSlot(slot, hasValue ? value.get() : keptValue.get());
    }
    DictEntry* entry = entryOf(target.get(), name);
    entry->writable = writable;
    entry->configurable = configurable;
    return true;
}

// ECMA-262 10.4.2.1, an ARRAY's [[DefineOwnProperty]], over a decoded
// descriptor. Three own-property stories, and only the last is ordinary:
//
//  - `length` is 10.4.2.4 ArraySetLength: the value is converted and checked
//    (the RangeError) before anything else, the shrink deletes the elements
//    above it, and `Object.freeze` is the only thing that has ever made it
//    non-writable. It is non-configurable and non-enumerable from birth, so a
//    descriptor asking for either is 10.1.6.3's refusal.
//  - an ELEMENT is a value in a dense block, not a descriptor. Its attributes
//    are all true, and the only thing that changes them is the array's
//    integrity level, which changes all of them at once (integrity.h). So a
//    descriptor that would leave one element with an attribute the others do
//    not have — a non-enumerable index, a non-writable one, an accessor at an
//    index — asks for storage bronze does not keep, and is refused BY NAME
//    rather than defined as something else; one that agrees with what the
//    element has is a value write, and one that asks a sealed or frozen
//    array to change is the TypeError 10.1.6.3 gives.
//  - anything else lives in the side object, which is an ordinary shape and
//    takes the ordinary algorithm — and carries the array's integrity level,
//    so a frozen array refuses a new named property there.
static bool applyArrayDescriptor(Rooted<Value>& self, PropertyKey name,
                                 const DecodedDescriptor& d, Rooted<Value>& value,
                                 Rooted<Value>& getter, Rooted<Value>& setter,
                                 bool throwOnRefusal) {
    const bool accessor = d.hasGet || d.hasSet;
    uint32_t index = 0;
    bool isLength = false;
    bool isIndex = false;
    if (name.isString()) {
        const std::string key = rtUtf8Chars(name.string());
        isLength = key == "length";
        isIndex = !isLength && rtIsIntegerLikeKey(key, index);
    }

    if (isLength) {
        if (accessor || (d.hasConfigurable && d.wantConfigurable) ||
            (d.hasEnumerable && d.wantEnumerable)) {
            return refuseDefine(throwOnRefusal, "Cannot redefine property: length");
        }
        const bool frozen = rtIntegrityLevel(self.get()) == IntegrityLevel::Frozen;
        if (d.hasWritable && d.wantWritable && frozen) {
            return refuseDefine(throwOnRefusal, "Cannot redefine property: length");
        }
        if (d.hasWritable && !d.wantWritable && !frozen) {
            fatal("unsupported: Object.defineProperty(array, 'length', { writable: false }) "
                  "(bronze records a non-writable `length` only as part of Object.freeze, "
                  "which makes every element non-writable with it)");
        }
        if (d.hasValue) {
            const SetRefusal refusal = rtArraySetLength(self, value.get());
            if (rtExceptionPending()) return false;
            if (refusal != SetRefusal::None) {
                return refuseDefine(throwOnRefusal, "Cannot redefine property: length");
            }
        }
        return true;
    }

    if (isIndex) {
        if (accessor) {
            fatal("unsupported: an accessor property at an array index (bronze keeps an "
                  "array's elements as values in a block, and an element cannot be a "
                  "getter/setter pair)");
        }
        auto* arr = self.get().asObject<ArrayHeader>();
        if (arr->hasElem(index)) {
            const bool writable =
                rtArrayElementWriteRefusal(self.get(), index) == SetRefusal::None;
            const bool configurable = rtArrayElementsConfigurable(self.get());
            // An attribute the element has that the descriptor would take
            // away is per-element storage bronze does not keep; one it lacks
            // that the descriptor would restore is 10.1.6.3 step 4's refusal
            // over a non-configurable property.
            if ((d.hasEnumerable && !d.wantEnumerable) || (d.hasWritable && !d.wantWritable && writable) ||
                (d.hasConfigurable && !d.wantConfigurable && configurable)) {
                fatal("unsupported: an array element with an attribute its neighbours lack "
                      "(bronze keeps one set of attributes per array, changed only by "
                      "Object.seal and Object.freeze)");
            }
            if ((d.hasWritable && d.wantWritable && !writable) ||
                (d.hasConfigurable && d.wantConfigurable && !configurable)) {
                return refuseDefine(throwOnRefusal, "Cannot redefine property: " + keyText(name));
            }
            if (d.hasValue) {
                if (!writable) {
                    // 4.e: a frozen element still accepts a redefinition that
                    // changes nothing.
                    if (!sameValue(arr->getElem(index), value.get())) {
                        return refuseDefine(throwOnRefusal,
                                            "Cannot redefine property: " + keyText(name));
                    }
                    return true;
                }
                arr->setElem(rtHeap(), index, value);
            }
            return true;
        }
        // A NEW element: 10.1.6.3 step 2 completes every absent attribute to
        // false, and an element with any of the three false is the storage
        // refused above.
        if (!(d.hasWritable && d.wantWritable && d.hasEnumerable && d.wantEnumerable &&
              d.hasConfigurable && d.wantConfigurable)) {
            fatal("unsupported: defining a new array element with an attribute false "
                  "(a descriptor that omits `writable`, `enumerable` or `configurable` "
                  "defaults it to false, and bronze keeps an element's attributes only as "
                  "the array's integrity level)");
        }
        if (rtArrayElementWriteRefusal(self.get(), index) == SetRefusal::NotExtensible) {
            return refuseDefine(throwOnRefusal, "Cannot define property, object is not extensible");
        }
        if (index > arr->length) {
            fatal("unsupported: defining an array element past `length` (a sparse array; "
                  "bronze keeps elements in a dense block)");
        }
        Rooted<Value> stored{d.hasValue ? value.get() : Value::fromUndefined()};
        self.get().asObject<ArrayHeader>()->setElem(rtHeap(), index, stored);
        return true;
    }

    Rooted<Value> holder{
        Value::fromObject(ArrayHeader::ensureProperties(rtHeap(), rtArena(), self))};
    return applyDecodedDescriptor(holder, name, d, value, getter, setter, throwOnRefusal);
}

// 6.2.6.4 FromPropertyDescriptor over a DECODED descriptor: a fresh ordinary
// object carrying exactly the fields the descriptor HAS, in the
// specification's field order. 10.5.6 step 9 hands a `defineProperty` trap
// this rather than the program's own descriptor object, so a trap sees the
// six-field shape and nothing the caller's object carried besides.
static Value descriptorObjectOf(const DecodedDescriptor& d, Rooted<Value>& value,
                                Rooted<Value>& getter, Rooted<Value>& setter) {
    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> flag{Value::fromUndefined()};
    if (d.hasValue) putField(out, "value", value);
    if (d.hasWritable) {
        flag.set(Value::fromBool(d.wantWritable));
        putField(out, "writable", flag);
    }
    if (d.hasGet) putField(out, "get", getter);
    if (d.hasSet) putField(out, "set", setter);
    if (d.hasEnumerable) {
        flag.set(Value::fromBool(d.wantEnumerable));
        putField(out, "enumerable", flag);
    }
    if (d.hasConfigurable) {
        flag.set(Value::fromBool(d.wantConfigurable));
        putField(out, "configurable", flag);
    }
    return out.get();
}

// 10.4.5.3 [[DefineOwnProperty]] on a typed array, for the NUMERIC half of
// its keys. A valid index takes a data descriptor whose attributes do not
// contradict the element's fixed `{ writable, enumerable, configurable: true }`
// and stores the value (step 1.b.iv-ix); an accessor, or any attribute set to
// false, is refused; an invalid index is refused outright (1.b.i). Returns
// false with `answered` clear for a name, which the shape then takes.
static bool applyTypedArrayDescriptor(Rooted<Value>& self, PropertyKey name,
                                      const DecodedDescriptor& d, Rooted<Value>& value,
                                      bool throwOnRefusal, bool& answered) {
    answered = false;
    if (!name.isString()) return false;
    const std::string key = rtUtf8Chars(name.string());
    uint32_t index = 0;
    const bool isIndex = rtIsIntegerLikeKey(key, index);
    if (!isIndex && !rtIsCanonicalNumericString(key)) return false;
    answered = true;
    if (!isIndex || index >= self.get().asObject<TypedArrayHeader>()->length) {
        return refuseDefine(throwOnRefusal, "Cannot define property " + key +
                                                ": a typed array has no such element");
    }
    if (d.hasGet || d.hasSet || (d.hasConfigurable && !d.wantConfigurable) ||
        (d.hasEnumerable && !d.wantEnumerable) || (d.hasWritable && !d.wantWritable)) {
        return refuseDefine(throwOnRefusal, "Cannot redefine property: " + key);
    }
    if (d.hasValue) {
        rtTypedArraySetElement(self, index, value.get());
        if (rtExceptionPending()) return false;
    }
    return true;
}

// 10.1.6.3, 10.4.2.1, 10.4.5.3 or 10.5.6, by the receiver's kind, over a table
// the receiver keeps somewhere: a function's statics box, an array's three
// stories above, a typed array's elements, a proxy's `defineProperty` trap,
// everything else its own shape.
static bool applyToReceiver(Rooted<Value>& self, PropertyKey name, const DecodedDescriptor& d,
                            Rooted<Value>& value, Rooted<Value>& getter, Rooted<Value>& setter,
                            bool throwOnRefusal) {
    const uint16_t kind = self.get().asObject<HeapObjectHeader>()->flags;
    if (kind == HeapKind::Array) {
        return applyArrayDescriptor(self, name, d, value, getter, setter, throwOnRefusal);
    }
    if (kind == HeapKind::TypedArray) {
        bool answered = false;
        const bool ok = applyTypedArrayDescriptor(self, name, d, value, throwOnRefusal, answered);
        if (answered) return ok;
    }
    if (kind == HeapKind::Proxy) {
        // The key as an ordinary heap string for the trap to hold, not the
        // arena's interned one.
        Rooted<Value> key{name.isSymbol() ? name.toValue() : rtKeyAsValue(name.string())};
        Rooted<Value> desc{descriptorObjectOf(d, value, getter, setter)};
        return rtProxyDefineOwnProperty(self.get(), key.get(), desc.get(), throwOnRefusal);
    }
    Rooted<Value> holder{self.get()};
    if (kind == HeapKind::Function) {
        rtEnsureFunctionProperties(self);
        holder.set(self.get().asObject<FunctionHeader>()->properties);
    }
    return applyDecodedDescriptor(holder, name, d, value, getter, setter, throwOnRefusal);
}

// ECMA-262 6.2.6.5 ToPropertyDescriptor followed by 10.1.6.3, which is what
// `Object.defineProperty` and `Reflect.defineProperty` are each a thin wrapper
// over. The errors of the DECODE — a non-object target, a descriptor that is
// not an object, a `get` that is not callable — are raised for both, and only
// the REFUSAL is the boolean `throwOnRefusal` chooses the meaning of.
bool rtObjectDefineOwnProperty(uint32_t argc, const uint64_t* argv, bool throwOnRefusal) {
    RootedArgs args(argc, argv);
    if (!rtObjectRequirePropertyTable(args[0], "defineProperty")) {
        return false;
    }
    Rooted<Value> self{args[0]};
    Rooted<Value> desc{args[2]};

    DecodedDescriptor d;
    Rooted<Value> value{Value::fromUndefined()};
    Rooted<Value> getter{Value::fromUndefined()};
    Rooted<Value> setter{Value::fromUndefined()};
    if (!decodeDescriptor(desc, d, value, getter, setter)) return false;

    // The key is built before the object is disturbed, and interned so the
    // entry can hold it forever.
    PropertyKey name = rtInternPropertyKey(args[1]);
    return applyToReceiver(self, name, d, value, getter, setter, throwOnRefusal);
}

// 20.1.2.4 Object.defineProperty: DefinePropertyOrThrow, so a refusal is the
// TypeError, and the answer is the target itself.
uint64_t rtObjectDefineProperty(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{args[0]};
    if (!rtObjectDefineOwnProperty(argc, argv, /*throwOnRefusal=*/true)) {
        return Value::fromUndefined().rawBits();
    }
    return self.get().rawBits();
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

    // 20.1.2.3.1 is TWO loops, and the split is observable: step 4 decodes
    // every descriptor (each field may be a getter, and any of them may
    // throw), step 5 defines them, and both are `?` — so the first abrupt
    // completion of the decode ends the operation before ANYTHING has been
    // defined. A batch either lands whole or not at all with respect to its
    // own decoding. The three payloads of each descriptor are parked in one
    // rooted block, because the remaining descriptors' getters run and
    // allocate between a decode and its apply.
    std::vector<DecodedDescriptor> decoded(count);
    std::vector<PropertyKey> names(count);
    RootedBlock payloads(count * 3);
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> key{keys.get().asObject<ArrayHeader>()->getElem(i)};
        Rooted<Value> desc{
            Value(bronze_elem_get(descriptors.get().rawBits(), key.get().rawBits()))};
        if (rtExceptionPending()) return false;
        Rooted<Value> value{Value::fromUndefined()};
        Rooted<Value> getter{Value::fromUndefined()};
        Rooted<Value> setter{Value::fromUndefined()};
        if (!decodeDescriptor(desc, decoded[i], value, getter, setter)) return false;
        payloads.set(i * 3, value.get());
        payloads.set(i * 3 + 1, getter.get());
        payloads.set(i * 3 + 2, setter.get());
        names[i] = rtInternPropertyKey(key.get());
    }
    for (uint32_t i = 0; i < count; ++i) {
        // Re-read from the block each turn: the collector keeps those slots
        // current across whatever the apply before this one allocated.
        Rooted<Value> value{Value(payloads.data()[i * 3])};
        Rooted<Value> getter{Value(payloads.data()[i * 3 + 1])};
        Rooted<Value> setter{Value(payloads.data()[i * 3 + 2])};
        // DefinePropertyOrThrow: 20.1.2.3.1 step 5.b.
        if (!applyToReceiver(target, names[i], decoded[i], value, getter, setter,
                             /*throwOnRefusal=*/true)) {
            return false;
        }
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

extern "C" {

// One key of an `Object.defineProperties(o, { k: { ... }, ... })` whose
// descriptors were all object literals, so 6.2.6.5's answer was a compile-time
// fact and `mask` is that fact (BRONZE_ABI_DESC_*).
//
// What it saves is the descriptor OBJECTS: three.js gives every `Object3D` six
// of them, each allocated, each then read back one field name at a time. What
// it must not change is anything else, so the target is checked with the same
// predicate and the same member name the generic member uses, and the define
// itself is 10.1.6.3 through the same `applyDecodedDescriptor` the decode
// hands its result to.
void bronze_define_own_attr(uint64_t objBits, uint32_t keyIndex, uint64_t valBits,
                            uint32_t mask) {
    Value objVal(objBits);
    // 20.1.2.3 step 1. The lowering emits a run of these for one call, and the
    // first of them stands where the member's own check stood, so a target
    // that is not an object throws before any key of the literal is defined.
    if (!rtObjectRequirePropertyTable(objVal, "defineProperties")) return;

    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("property definition with an unregistered key index");

    Rooted<Value> self{objVal};
    Rooted<Value> value{Value(valBits)};

    // An inherited descriptor field is a real program, and the only way to keep
    // it observable is to let the generic member see it: the fields the literal
    // wrote go into a real descriptor object, whose prototype is the polluted
    // one, and 6.2.6.5 reads it as it would have.
    if (!literalDescriptorFieldsAreOwnOnly()) {
        Rooted<Value> key{Value::fromString(keyHeader)};
        Rooted<Value> desc{Value(bronze_create_object())};
        if (mask & BRONZE_ABI_DESC_HAS_VALUE) putField(desc, "value", value);
        Rooted<Value> flag{Value::fromUndefined()};
        if (mask & BRONZE_ABI_DESC_HAS_WRITABLE) {
            flag.set(Value::fromBool((mask & BRONZE_ABI_DESC_WRITABLE) != 0));
            putField(desc, "writable", flag);
        }
        if (mask & BRONZE_ABI_DESC_HAS_ENUMERABLE) {
            flag.set(Value::fromBool((mask & BRONZE_ABI_DESC_ENUMERABLE) != 0));
            putField(desc, "enumerable", flag);
        }
        if (mask & BRONZE_ABI_DESC_HAS_CONFIGURABLE) {
            flag.set(Value::fromBool((mask & BRONZE_ABI_DESC_CONFIGURABLE) != 0));
            putField(desc, "configurable", flag);
        }
        const uint64_t call[3] = {self.get().rawBits(), key.get().rawBits(),
                                  desc.get().rawBits()};
        rtObjectDefineProperty(0, 0, 3, call);
        return;
    }

    DecodedDescriptor d;
    d.hasValue = (mask & BRONZE_ABI_DESC_HAS_VALUE) != 0;
    d.hasWritable = (mask & BRONZE_ABI_DESC_HAS_WRITABLE) != 0;
    d.hasEnumerable = (mask & BRONZE_ABI_DESC_HAS_ENUMERABLE) != 0;
    d.hasConfigurable = (mask & BRONZE_ABI_DESC_HAS_CONFIGURABLE) != 0;
    d.wantWritable = (mask & BRONZE_ABI_DESC_WRITABLE) != 0;
    d.wantEnumerable = (mask & BRONZE_ABI_DESC_ENUMERABLE) != 0;
    d.wantConfigurable = (mask & BRONZE_ABI_DESC_CONFIGURABLE) != 0;

    // No accessor half can reach here: the mask has no bits for `get` or `set`,
    // and the lowering refuses a descriptor literal that names either.
    Rooted<Value> noGetter{Value::fromUndefined()};
    Rooted<Value> noSetter{Value::fromUndefined()};
    // 20.1.2.3.1 step 5 is DefinePropertyOrThrow, so a refusal is 20.1.2.4's
    // TypeError and the keys already defined stay defined.
    applyToReceiver(self, PropertyKey::forString(keyHeader), d, value, noGetter, noSetter,
                    /*throwOnRefusal=*/true);
}

}  // extern "C"

}  // namespace bronze::runtime
