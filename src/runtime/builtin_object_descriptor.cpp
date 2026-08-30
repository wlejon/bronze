
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

// ---- reading a descriptor's six fields --------------------------------------
//
// 6.2.6.5 ToPropertyDescriptor asks HasProperty and then Get, once for each of
// six field names. A constructor decodes one descriptor per object it makes —
// three.js gives every `Object3D` six through `Object.defineProperties` and a
// seventh through `defineProperty` — so the decode is not a cold path, and
// spelled as the two generic calls it was mostly string work: `rtMakeString`
// built the field's name, `bronze_has_property` converted it back to text and
// built a SECOND heap string from that, walked, and `getProp` walked again.
// Two heap strings, a `std::string` and two chain walks per field, before the
// descriptor's own contents were looked at.
//
// The names are six constants. They go through the key registry, which interns
// by text and hands back the immortal arena string it already holds — so the
// decode allocates nothing, and the two questions are answered in ONE walk,
// because for an ordinary descriptor both answers are the slot the walk finds.
// A shape key is matched by content (`PropertyKey::matches`), so what the walk
// spends per link is a length test and a memcmp of at most twelve bytes.

// The six fields, in the order 6.2.6.5 reads them. That order is OBSERVABLE —
// a descriptor may spell a field as a getter, and one getter may add or change
// a field the decode has not reached yet — so it is the specification's and not
// a convenience. `kDescFieldNames` is indexed by this enum.
enum class DescField : uint8_t { Enumerable, Configurable, Value, Writable, Get, Set };
constexpr size_t kDescFieldCount = 6;
constexpr const char* kDescFieldNames[kDescFieldCount] = {"enumerable", "configurable", "value",
                                                          "writable",   "get",          "set"};

// BRONZE_NO_DESC_FIELDS=1 puts the decode back on a freshly built name and the
// pair of generic calls, so one binary A/Bs the whole of the above.
bool descFieldFastPath() {
    static const bool enabled = []() {
        const char* env = std::getenv("BRONZE_NO_DESC_FIELDS");
        return !(env && std::strcmp(env, "1") == 0);
    }();
    return enabled;
}

// The field's name as the immortal arena string the key registry holds for it.
// The registry is per-thread and so is this memo of it; an arena string never
// moves and is never freed, so holding the header is the same promise
// `rtKeyHeader` already makes to the property path.
PropertyKey descFieldKey(DescField field) {
    static thread_local StringHeader* memo[kDescFieldCount] = {};
    const size_t i = static_cast<size_t>(field);
    if (memo[i] == nullptr) {
        memo[i] = rtKeyHeader(bronze_register_key_string(kDescFieldNames[i]));
    }
    return PropertyKey::forString(memo[i]);
}

// What one walk of the descriptor's chain could answer.
enum class FieldFound : uint8_t { Present, Absent, Generic };

// HasProperty and Get over `desc`'s prototype chain, in one walk.
//
// The walk is `plainObjectHas`'s (rt_operator.cpp) step for step, because that
// is the walk `bronze_has_property` reaches for a plain receiver and the
// presence answer has to be the same answer. Where the walk cannot also produce
// what Get would return — an ACCESSOR, whose Get runs user code, or a holder
// that is not an ordinary object, whose value does not come out of a slot — it
// answers `Generic` and the caller runs the two generic calls as before.
FieldFound lookupField(Value descVal, PropertyKey name, Value& out) {
    auto* hdr = descVal.asObject<HeapObjectHeader>();
    for (uint32_t depth = 0; depth <= 1000; ++depth) {
        auto* holder = reinterpret_cast<ObjectHeader*>(hdr);
        PropertyInfo info;
        if (holder->shape != nullptr && holder->shape->lookupProperty(name, info)) {
            if (info.accessor || hdr->flags != HeapKind::Plain) return FieldFound::Generic;
            out = holder->getSlot(info.slot);
            return FieldFound::Present;
        }
        ObjectHeader* next = holder->protoAncestor(1);
        if (next == nullptr) return FieldFound::Absent;
        hdr = reinterpret_cast<HeapObjectHeader*>(next);
    }
    fatal("prototype chain too deep (a cycle?)");
}

// `ordinary` is false for a String wrapper, which answers `length` and its
// indices beside its shape and so is not fully described by the walk above.
// None of the six names is such a key, but that is a fact about the RECEIVER
// and is asked once per descriptor rather than assumed six times.
Value readField(Rooted<Value>& desc, DescField field, bool ordinary, bool& present) {
    // The key is taken BEFORE the descriptor is read: interning one the first
    // time a thread asks for it allocates, and an argument list gives no order
    // to evaluate the two in — so a raw `desc.get()` beside the call could be
    // the address the collector had just moved away from.
    const PropertyKey name = descFieldKey(field);
    if (ordinary && descFieldFastPath()) {
        Value out;
        switch (lookupField(desc.get(), name, out)) {
            case FieldFound::Present:
                present = true;
                return out;
            case FieldFound::Absent:
                present = false;
                return Value::fromUndefined();
            case FieldFound::Generic:
                break;
        }
    }
    Rooted<Value> key{descFieldFastPath()
                          ? name.toValue()
                          : rtMakeString(kDescFieldNames[static_cast<size_t>(field)])};
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

// A [[DefineOwnProperty]] that REFUSED. 20.1.2.4 `Object.defineProperty` turns
// the refusal into a TypeError (step 4 is `DefinePropertyOrThrow`); 28.1.3
// `Reflect.defineProperty` returns the boolean instead and must not throw for
// it. So a refusal is a VALUE here and becomes a throw only at the entry point
// that asked for one.
bool refuseDefine(bool throwOnRefusal, const std::string& message) {
    if (throwOnRefusal) rtThrowTypeError(message);
    return false;
}

// A descriptor after 6.2.6.5 has run, or as a compile-time literal already
// settled it: which of the six fields the descriptor HAS, and the three
// attribute booleans it wanted. The `value`, `get` and `set` payloads travel
// separately because they are Values and have to be rooted by their caller.
//
// It is the seam between the two halves of 10.1.6.3's one caller. Reading a
// descriptor object is a program the descriptor's author wrote (each field may
// be a getter, and the order is observable); applying the result is the
// specification's own algorithm over storage. Only the first half has anything
// to do with objects, which is what lets a caller that already knows the
// answer — `bronze_define_own_attr` — skip it without a second copy of the
// second half drifting away from this one.
struct DecodedDescriptor {
    bool hasValue = false;
    bool hasWritable = false;
    bool hasEnumerable = false;
    bool hasConfigurable = false;
    bool hasGet = false;
    bool hasSet = false;
    bool wantWritable = false;
    bool wantEnumerable = false;
    bool wantConfigurable = false;
};

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
    if (!rtObjectIsPlain(args[2])) {
        rtThrowTypeError("Property description must be an object");
        return false;
    }
    Rooted<Value> self{args[0]};
    Rooted<Value> desc{args[2]};

    Rooted<Value> target{self.get()};
    if (self.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        rtEnsureFunctionProperties(self);
        target.set(self.get().asObject<FunctionHeader>()->properties);
    }

    DecodedDescriptor d;
    Value wrapped;
    const bool ordinary = !rtStringWrapperData(desc.get(), wrapped);
    // 6.2.6.5 steps 3 through 8, in the order it states them. The order is
    // observable whenever a field is spelled as a getter: one such getter can
    // see which fields have already been asked for, and can add a field the
    // decode has not reached yet, so reading `value` before `enumerable` is a
    // different program than reading it after.
    //
    // Every one of the six is spelled `? HasProperty(...)` / `? Get(...)`, and
    // the `?` is the reason for the test between each pair. A field getter that
    // throws — or a Proxy descriptor's `has` trap — is an ABRUPT COMPLETION:
    // 6.2.6.5 returns it, so the remaining fields are never read (their getters
    // must not run) and 10.1.6.3 never runs at all. `readField` has no way to
    // spell an abrupt completion — it answers `undefined`, which is also what a
    // present field holding `undefined` looks like — so the completion is the
    // pending exception, and continuing past it defined a property out of a
    // descriptor the program never finished handing over.
    Rooted<Value> enumerableV{readField(desc, DescField::Enumerable, ordinary, d.hasEnumerable)};
    if (rtExceptionPending()) return false;
    Rooted<Value> configurableV{
        readField(desc, DescField::Configurable, ordinary, d.hasConfigurable)};
    if (rtExceptionPending()) return false;
    Rooted<Value> value{readField(desc, DescField::Value, ordinary, d.hasValue)};
    if (rtExceptionPending()) return false;
    Rooted<Value> writableV{readField(desc, DescField::Writable, ordinary, d.hasWritable)};
    if (rtExceptionPending()) return false;
    Rooted<Value> getter{readField(desc, DescField::Get, ordinary, d.hasGet)};
    if (rtExceptionPending()) return false;
    Rooted<Value> setter{readField(desc, DescField::Set, ordinary, d.hasSet)};
    if (rtExceptionPending()) return false;

    // 6.2.6.5 steps 7.c and 8.c: a `get` or `set` that is PRESENT and is
    // neither callable nor `undefined` does not describe an accessor, so the
    // descriptor is rejected before anything is defined. This is an error of
    // the DECODE and not a refusal — `Reflect.defineProperty` raises it too.
    if (d.hasGet && !getter.get().isUndefined() && !rtIsCallableValue(getter.get())) {
        rtThrowTypeError("Getter must be a function");
        return false;
    }
    if (d.hasSet && !setter.get().isUndefined() && !rtIsCallableValue(setter.get())) {
        rtThrowTypeError("Setter must be a function");
        return false;
    }

    if ((d.hasGet || d.hasSet) && (d.hasValue || d.hasWritable)) {
        rtThrowTypeError(
            "Invalid property descriptor. Cannot both specify accessors and a value or "
            "writable attribute");
        return false;
    }
    d.wantWritable = d.hasWritable && bronze_truthy(writableV.get().rawBits());
    d.wantEnumerable = d.hasEnumerable && bronze_truthy(enumerableV.get().rawBits());
    d.wantConfigurable = d.hasConfigurable && bronze_truthy(configurableV.get().rawBits());

    // The key is built before the object is disturbed, and interned so the
    // entry can hold it forever.
    PropertyKey name = rtInternPropertyKey(args[1]);
    return applyDecodedDescriptor(target, name, d, value, getter, setter, throwOnRefusal);
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

namespace {

// Are the six descriptor fields of a fresh object literal exactly the ones the
// literal WROTE?
//
// 6.2.6.5 reads each field with HasProperty and Get, and both walk the
// descriptor's prototype chain — so `{ value: v }` describes a non-enumerable
// property only while nothing on `Object.prototype` answers `enumerable`. A
// program that puts one there has changed what every descriptor literal in it
// means, and a lowering that read the literal's own text would not have
// noticed.
//
// The answer is memoized on the prototype's SHAPE, which is what makes the
// question affordable: a shape is immutable and installing a property
// transitions to a different one, so a pointer compare is a proof that nothing
// was added since the walk. Two conditions narrow where that proof holds — a
// dictionary shape is mutated in place, and the memo answers for the whole
// chain rather than for one link — and outside them the six lookups are
// simply repeated.
bool literalDescriptorFieldsAreOwnOnly() {
    // The keys first: interning one allocates the first time a thread asks for
    // it, and an allocation moves the object the walk below holds a pointer to.
    PropertyKey fields[kDescFieldCount];
    for (size_t f = 0; f < kDescFieldCount; ++f) {
        fields[f] = descFieldKey(static_cast<DescField>(f));
    }

    const Value protoVal = rtObjectPrototype();
    if (!protoVal.isObject()) return true;
    auto* proto = protoVal.asObject<ObjectHeader>();

    static thread_local Shape* verified = nullptr;
    const bool memoizable = proto->shape != nullptr && !proto->shape->isDictionary() &&
                            proto->protoAncestor(1) == nullptr;
    if (memoizable && proto->shape == verified) return true;

    ObjectHeader* link = proto;
    for (uint32_t depth = 0; link != nullptr && depth <= 1000; ++depth) {
        if (link->shape != nullptr) {
            PropertyInfo info;
            for (const PropertyKey& key : fields) {
                if (link->shape->lookupProperty(key, info)) return false;
            }
        }
        link = link->protoAncestor(1);
    }
    if (memoizable) verified = proto->shape;
    return true;
}

}  // namespace

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

    Rooted<Value> target{self.get()};
    if (self.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        rtEnsureFunctionProperties(self);
        target.set(self.get().asObject<FunctionHeader>()->properties);
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
    applyDecodedDescriptor(target, PropertyKey::forString(keyHeader), d, value, noGetter,
                           noSetter, /*throwOnRefusal=*/true);
}

}  // extern "C"

}  // namespace bronze::runtime
