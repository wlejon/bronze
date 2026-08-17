// The essential invariants of the Proxy exotic object (ECMA-262 10.5) — the
// second half of every internal method in proxy.cpp.
//
// A trap is user code and may answer anything. What keeps a proxy from being a
// hole in the object model is that each internal method then asks the TARGET
// the same question and refuses an answer that contradicts it. The rules all
// come from one place, 6.1.7.3's list of invariants of the essential internal
// methods, and they protect exactly two things:
//
//   NON-CONFIGURABILITY. A property that cannot be reconfigured cannot be made
//   to look reconfigured. A non-writable non-configurable data property has one
//   value forever, so `get` must answer it and `set` must not claim to have
//   changed it; a non-configurable property exists forever, so `has` must not
//   deny it and `deleteProperty` must not claim to have removed it.
//
//   NON-EXTENSIBILITY. A target that can gain no property can gain none through
//   a proxy either, and its own-key list and its prototype are then both fixed:
//   `ownKeys` must report exactly the target's keys and `getPrototypeOf` must
//   report the target's prototype.
//
// These live in their own file rather than in proxy.cpp because they are one
// SUBJECT — every function here reads the target and throws, none of them calls
// a trap — and because proxy.cpp is already the length it wants to be.
//
// WHERE THE TARGET'S ANSWER COMES FROM. `rtOwnPropertyOf`, the one own-property
// switch in the runtime (builtin_object_proto.cpp). Not
// `Object.getOwnPropertyDescriptor`: that member refuses by name for a receiver
// whose own properties are not in a shape — an array being the common one — and
// asking it here would turn `new Proxy([1, 2], { get() {} })`, which works
// today, into a hard error. The switch answers for every kind, and the two
// attributes these checks gate on default to the PERMISSIVE value for a kind
// whose storage cannot express them, so a gap can only ever fail to fire a
// check and never fire a wrong one.
//
// EVERY FUNCTION HERE IS A GC POINT. `rtOwnPropertyOf` on a proxy target runs
// that proxy's own trap, and building a message allocates a string. So each
// takes its operands as roots, and each re-reads through them.
#include <cmath>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_object.h"
#include "runtime/exception.h"
#include "runtime/integrity.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// 7.2.11 SameValue, which is `===` with the two numeric corrections: NaN is the
// same value as NaN, and +0 is NOT the same value as -0. Every invariant that
// compares a trap's answer against a stored one uses it — `Object.is`'s
// relation, not `===`'s — so a `get` trap answering -0 for a frozen +0 is the
// TypeError 10.5.8 step 10 names.
bool sameValue(Value a, Value b) {
    if (a.isNumber() && b.isNumber()) {
        const double x = a.asNumber();
        const double y = b.asNumber();
        if (std::isnan(x) && std::isnan(y)) return true;
        if (x == 0.0 && y == 0.0) return std::signbit(x) == std::signbit(y);
        return x == y;
    }
    // Every non-numeric case is SameValueZero's: a string by contents, a BigInt
    // by value, everything else by identity. The two relations differ on
    // nothing but the zeroes, so the rest is not written twice.
    return sameValueZero(a, b);
}

// A key as text for a diagnostic. `Symbol(desc)` for a symbol — the only
// spelling one has (20.4.3.3.1).
std::string keyText(Rooted<Value>& key) {
    if (key.get().isSymbol()) return rtSymbolDescriptiveString(key.get());
    Rooted<Value> str{rtValueToString(key.get())};
    if (rtExceptionPending()) return std::string("<key>");
    return rtUtf8Chars(str.get().asString<StringHeader>());
}

// The one sentence shape every refusal here takes: which trap, which key, and
// what about the target it contradicted.
void refuse(const char* trap, Rooted<Value>& key, const char* because) {
    rtThrowTypeError(std::string("'") + trap + "' on proxy: trap reported " + because +
                     " for property '" + keyText(key) + "'");
}

// The target's own property, through the runtime's single own-property switch.
// False means absent — or that a nested proxy's trap threw, which the caller
// separates by testing the pending cell.
bool targetOwn(Rooted<Value>& target, Rooted<Value>& key, OwnPropertyDetail& out) {
    return rtOwnPropertyOf(target, key.get(), out);
}

// 6.2.6.6 ValidateAndApplyPropertyDescriptor with O undefined, which is what
// 6.2.6.7 IsCompatiblePropertyDescriptor is. `desc` has already been through
// CompletePropertyDescriptor, so every field of its kind is present — which is
// why this reads them all unconditionally where the general algorithm asks
// "if Desc has a [[Writable]] field".
bool compatibleDescriptor(bool extensibleTarget, const OwnPropertyDetail& desc,
                          bool targetExists, const OwnPropertyDetail& current) {
    // Step 2: nothing to be compatible with. A new property needs only room.
    if (!targetExists) return extensibleTarget;
    // Step 4 onward applies to a non-configurable current property; a
    // configurable one may be redescribed into anything at all.
    if (current.configurable) return true;
    if (desc.configurable) return false;
    if (desc.enumerable != current.enumerable) return false;
    if (desc.accessor != current.accessor) return false;
    if (current.accessor) {
        return sameValue(desc.getter, current.getter) &&
               sameValue(desc.setter, current.setter);
    }
    if (current.writable) return true;
    if (desc.writable) return false;
    // The value is compared only when the target's is KNOWN; where producing it
    // would have allocated (builtin_object.h says which receivers those are)
    // this check stands down rather than comparing against a stand-in.
    return !current.valueKnown || sameValue(desc.value, current.value);
}

// 6.2.6.5 ToPropertyDescriptor followed by 6.2.6.6 CompletePropertyDescriptor,
// over the object a `getOwnPropertyDescriptor` trap returned. Every read is an
// ordinary [[Get]] and can run user code, so the object arrives as a root and
// the caller tests the pending cell.
//
// COMPLETION is what makes the result comparable: a descriptor a trap wrote as
// `{ value: 1 }` means writable, enumerable and configurable FALSE once
// completed, which is the difference between 10.5.5's checks passing and
// failing.
bool decodeDescriptor(Rooted<Value>& descObj, OwnPropertyDetail& out) {
    out = OwnPropertyDetail{};
    Rooted<Value> field;
    const auto present = [&](const char* name) -> bool {
        Rooted<Value> nameKey{rtMakeString(name)};
        if (!bronze_has_property(nameKey.get().rawBits(), descObj.get().rawBits())) return false;
        field.set(Value(bronze_elem_get(descObj.get().rawBits(), nameKey.get().rawBits())));
        return !rtExceptionPending();
    };

    bool hasGet = false, hasSet = false;
    Rooted<Value> getter;
    Rooted<Value> setter;
    Rooted<Value> value;
    if (present("get")) {
        hasGet = true;
        getter.set(field.get());
    }
    if (rtExceptionPending()) return false;
    if (present("set")) {
        hasSet = true;
        setter.set(field.get());
    }
    if (rtExceptionPending()) return false;
    if (present("value")) value.set(field.get());
    if (rtExceptionPending()) return false;

    bool writable = false, enumerable = false, configurable = false;
    if (present("writable")) writable = bronze_truthy(field.get().rawBits());
    if (rtExceptionPending()) return false;
    if (present("enumerable")) enumerable = bronze_truthy(field.get().rawBits());
    if (rtExceptionPending()) return false;
    if (present("configurable")) configurable = bronze_truthy(field.get().rawBits());
    if (rtExceptionPending()) return false;

    out.accessor = hasGet || hasSet;
    out.enumerable = enumerable;
    out.configurable = configurable;
    if (out.accessor) {
        out.getter = getter.get();
        out.setter = setter.get();
        // CompletePropertyDescriptor leaves [[Writable]] off an accessor
        // entirely; nothing below reads it for one.
        out.writable = false;
    } else {
        out.writable = writable;
        out.value = value.get();
        out.valueKnown = true;
    }
    return true;
}

}  // namespace

void rtProxyCheckGet(Rooted<Value>& target, Rooted<Value>& key, Rooted<Value>& trapResult) {
    OwnPropertyDetail current;
    if (!targetOwn(target, key, current) || rtExceptionPending()) return;
    if (current.configurable) return;
    if (!current.accessor) {
        // 10.5.8 step 10.a: a frozen data property reads one value forever.
        if (current.writable || !current.valueKnown) return;
        if (!sameValue(trapResult.get(), current.value)) {
            refuse("get", key,
                   "a value different from the non-writable, non-configurable one the target "
                   "holds");
        }
        return;
    }
    // Step 10.b: an accessor with no getter reads `undefined` forever, so a
    // trap may not invent a value for it.
    if (current.getter.isUndefined() && !trapResult.get().isUndefined()) {
        refuse("get", key,
               "a value for a non-configurable accessor on the target whose getter is undefined");
    }
}

void rtProxyCheckSet(Rooted<Value>& target, Rooted<Value>& key, Rooted<Value>& value) {
    OwnPropertyDetail current;
    if (!targetOwn(target, key, current) || rtExceptionPending()) return;
    if (current.configurable) return;
    if (!current.accessor) {
        // 10.5.9 step 9.a: a write of the SAME value is not a change, so it is
        // still a legal success.
        if (current.writable || !current.valueKnown) return;
        if (!sameValue(value.get(), current.value)) {
            refuse("set", key,
                   "a successful write of a value different from the non-writable, "
                   "non-configurable one the target holds");
        }
        return;
    }
    // Step 9.b: a setter-less accessor cannot be written at all.
    if (current.setter.isUndefined()) {
        refuse("set", key,
               "a successful write to a non-configurable accessor on the target whose setter is "
               "undefined");
    }
}

void rtProxyCheckHas(Rooted<Value>& target, Rooted<Value>& key, bool trapAnswer) {
    // 10.5.7 step 9 runs only for a trap that answered FALSE: a trap may report
    // a property the target has not got, and only a DENIAL can contradict it.
    if (trapAnswer) return;
    OwnPropertyDetail current;
    if (!targetOwn(target, key, current) || rtExceptionPending()) return;
    if (!current.configurable) {
        refuse("has", key, "the absence of a non-configurable property of the target");
        return;
    }
    if (!rtIsExtensible(target.get())) {
        refuse("has", key,
               "the absence of an own property of a target that is not extensible");
    }
}

void rtProxyCheckDelete(Rooted<Value>& target, Rooted<Value>& key, bool trapAnswer) {
    // 10.5.10 step 10: a trap that answered false refused the delete, and a
    // refusal cannot contradict anything.
    if (!trapAnswer) return;
    OwnPropertyDetail current;
    if (!targetOwn(target, key, current) || rtExceptionPending()) return;
    if (!current.configurable) {
        refuse("deleteProperty", key,
               "the removal of a non-configurable property of the target");
        return;
    }
    if (!rtIsExtensible(target.get())) {
        refuse("deleteProperty", key,
               "the removal of an own property of a target that is not extensible");
    }
}

void rtProxyCheckGetOwnProperty(Rooted<Value>& target, Rooted<Value>& key,
                                Rooted<Value>& desc) {
    OwnPropertyDetail current;
    const bool exists = targetOwn(target, key, current);
    if (rtExceptionPending()) return;
    // The target's value may be a heap value the message-building below moves;
    // it is only ever compared, never held, so nothing needs rooting.
    const bool extensible = rtIsExtensible(target.get());

    if (desc.get().isUndefined()) {
        // Steps 10-12: reporting a property absent is a lie if the target's is
        // non-configurable, or if the target could never lose it.
        if (!exists) return;
        if (!current.configurable) {
            refuse("getOwnPropertyDescriptor", key,
                   "no descriptor for a non-configurable property of the target");
            return;
        }
        if (!extensible) {
            refuse("getOwnPropertyDescriptor", key,
                   "no descriptor for an own property of a target that is not extensible");
        }
        return;
    }

    OwnPropertyDetail reported;
    if (!decodeDescriptor(desc, reported) || rtExceptionPending()) return;

    // Step 16: the descriptor must be one the target could actually have been
    // redescribed into.
    if (!compatibleDescriptor(extensible, reported, exists, current)) {
        refuse("getOwnPropertyDescriptor", key,
               "a descriptor incompatible with the one the target holds");
        return;
    }
    // Step 17: a trap may not manufacture non-configurability.
    if (reported.configurable) return;
    if (!exists) {
        refuse("getOwnPropertyDescriptor", key,
               "a non-configurable descriptor for a property the target has not got");
        return;
    }
    if (current.configurable) {
        refuse("getOwnPropertyDescriptor", key,
               "a non-configurable descriptor for a configurable property of the target");
        return;
    }
    if (!reported.accessor && !reported.writable && current.writable) {
        refuse("getOwnPropertyDescriptor", key,
               "a non-configurable, non-writable descriptor for a writable property of the "
               "target");
    }
}

namespace {

// Whether `keys` — an Array of property keys — holds `key`, by the same
// relation a property key is compared with anywhere else: a string matches on
// contents and a symbol on identity, which is what `bronze_strict_equals`
// already means for the two.
bool listHasKey(Rooted<Value>& keys, Value key) {
    ArrayHeader* arr = keys.get().asObject<ArrayHeader>();
    const uint32_t count = arr->length;
    for (uint32_t i = 0; i < count; ++i) {
        if (sameValueZero(arr->getElem(i), key)) return true;
    }
    return false;
}

}  // namespace

void rtProxyCheckOwnKeys(Rooted<Value>& target, Rooted<Value>& keys) {
    // Step 10's duplicate check comes first and is unconditional: a key list
    // with a repeat is a TypeError however extensible the target is, because
    // 6.1.7.1 makes an own-key list a list of DISTINCT keys.
    {
        ArrayHeader* arr = keys.get().asObject<ArrayHeader>();
        const uint32_t count = arr->length;
        for (uint32_t i = 0; i < count; ++i) {
            for (uint32_t j = i + 1; j < count; ++j) {
                if (sameValueZero(arr->getElem(i), arr->getElem(j))) {
                    rtThrowTypeError(
                        "'ownKeys' on proxy: trap returned a list with a duplicate key");
                    return;
                }
            }
        }
    }

    const bool extensible = rtIsExtensible(target.get());
    // The target's OWN keys, through the SAME answer the forward path of
    // 10.5.11 hands back when a handler has no `ownKeys` trap. Deliberately not
    // `Object.getOwnPropertyNames`: that member refuses by name for receiver
    // kinds a proxy target may well be, and a check is not allowed to be
    // stricter about the target than the forward it guards.
    Rooted<Value> targetKeys{rtProxyTargetOwnKeys(target)};
    if (rtExceptionPending()) return;

    // Steps 13-15: the split, and the early exit that is the whole reason an
    // ordinary extensible target costs a proxy nothing here.
    bool anyNonConfigurable = false;
    const uint32_t targetCount = targetKeys.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < targetCount; ++i) {
        Rooted<Value> key{targetKeys.get().asObject<ArrayHeader>()->getElem(i)};
        OwnPropertyDetail current;
        const bool exists = targetOwn(target, key, current);
        if (rtExceptionPending()) return;
        if (!exists || current.configurable) continue;
        anyNonConfigurable = true;
        // Step 19: a non-configurable key cannot be hidden.
        if (!listHasKey(keys, key.get())) {
            refuse("ownKeys", key,
                   "a key list omitting a non-configurable own property of the target");
            return;
        }
    }
    if (extensible && !anyNonConfigurable) return;
    if (extensible) return;

    // Steps 21-22, for a target that can gain nothing: every own key must
    // appear, and nothing else may.
    for (uint32_t i = 0; i < targetCount; ++i) {
        Rooted<Value> key{targetKeys.get().asObject<ArrayHeader>()->getElem(i)};
        if (!listHasKey(keys, key.get())) {
            refuse("ownKeys", key,
                   "a key list omitting an own property of a target that is not extensible");
            return;
        }
    }
    const uint32_t reportedCount = keys.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < reportedCount; ++i) {
        Rooted<Value> key{keys.get().asObject<ArrayHeader>()->getElem(i)};
        if (!listHasKey(targetKeys, key.get())) {
            refuse("ownKeys", key,
                   "a key the target does not own, for a target that is not extensible");
            return;
        }
    }
}

void rtProxyCheckPrototype(Rooted<Value>& target, Rooted<Value>& trapResult) {
    // 10.5.1 step 7: an extensible target's prototype can still change, so the
    // trap is free.
    if (rtIsExtensible(target.get())) return;
    const uint64_t call[1] = {target.get().rawBits()};
    Value actual = Value(objectGetPrototypeOf(0, 0, 1, call));
    if (rtExceptionPending()) return;
    if (!sameValue(trapResult.get(), actual)) {
        rtThrowTypeError(
            "'getPrototypeOf' on proxy: trap returned a prototype different from the one held "
            "by a target that is not extensible");
    }
}

}  // namespace bronze::runtime
