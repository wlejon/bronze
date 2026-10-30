// The DEFINITION half of the property write path: what an object literal's
// `k: v`, `{ k }`, `{ [e]: v }`, `{ k() {} }`, an object spread and a class
// field each store with. ECMA-262 spells every one of them
// CreateDataPropertyOrThrow (7.3.7, through 13.2.5.5 PropertyDefinitionEvaluation,
// 14.3.1 CopyDataProperties and 7.3.33 DefineField), and NONE of them is an
// assignment: no inherited setter runs, no inherited read-only property refuses
// the store, no proxy on the prototype chain is asked. `({ q: 1 })` under an
// `Object.prototype` that has `set q(v)` has an own `q`, and the setter never
// hears of it.
//
// Its own helpers rather than a flag on bronze_prop_set because the two must
// not SHARE anything a later store could replay: a `prop.set` site's inline
// cache can hold an inherited accessor (a setter to call) and its transition
// hit is guarded by the prototype epoch an inherited setter would bump; a
// define site's cache must hold neither. Every cache a define site owns is
// filled here and only here, and only with what a define may repeat: an own
// data property whose attributes are the three trues a define would give it
// (so a slot store is the whole define), or the add of exactly that property.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/builtin_object.h"
#include "runtime/dictionary.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/property_key.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/shape_census.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// Is the property already on `obj` one a define may simply overwrite? Only an
// own DATA property that is writable, enumerable and configurable: 10.1.6.3
// over the complete descriptor changes nothing about it but its value. Any
// other own property (an accessor, a non-writable or non-enumerable one) is
// the general case — a redefinition, or a refusal for a non-configurable one.
bool ownIsPlainData(const PropertyInfo& p) {
    return !p.accessor && p.writable && p.enumerable && p.configurable;
}

}  // namespace

// 7.3.7 CreateDataPropertyOrThrow(target, name, value). `ic` is the define
// site's own cache entry or null. Returns false when it threw.
bool rtCreateDataPropertyOrThrow(Rooted<Value>& target, PropertyKey name, Rooted<Value>& value,
                                 InlineCache* ic) {
    if (!target.get().isObject()) {
        // Every caller hands an object it made or a constructor's receiver;
        // anything else is a lowering bug, not a program.
        fatal("internal: a data-property definition on a value that is not an object");
    }
    auto* hdr = target.get().asObject<HeapObjectHeader>();
    if (hdr->flags == HeapKind::Plain) {
        auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
        Shape* shape = obj->shape;
        // The site's own entry: filled below only for an own all-true data
        // property, so a shape match is the whole define.
        if (ic && shape && ic->describesOwn(shape)) {
            obj->setSlot(ic->cached_slot, value.get());
            return true;
        }
        // A String exotic object's characters and `length` are own,
        // non-configurable properties its shape does not record (10.4.3); the
        // general path below refuses them.
        Value stringData;
        const bool stringWrapper = rtStringWrapperData(target.get(), stringData);
        if (shape && !stringWrapper && !rtArrayPrototypeRefusesChange(target.get())) {
            bool present = false;
            bool plainData = false;
            if (shape->isDictionary()) {
                if (const DictEntry* e = shape->dict->find(name)) {
                    present = true;
                    plainData = !e->accessor && e->writable && e->enumerable && e->configurable;
                }
            } else {
                PropertyInfo own;
                present = shape->lookupProperty(name, own);
                plainData = present && ownIsPlainData(own);
            }
            if (!present || plainData) {
                // setProp's define form: no chain walk, and for a present key
                // a slot store (it may fill `ic` with the own slot — all-true by
                // the test above); for an absent one an add of an enumerable,
                // writable, configurable data property (which it may publish to
                // `ic` as a transition). The only refusal it can answer is a
                // non-extensible receiver, which 7.3.7 raises.
                Rooted<Value> key{name.toValue()};
                SetRefusal refusal = SetRefusal::None;
                target.get().asObject<ObjectHeader>()->setProp(
                    rtHeap(), rtArena(), key, value, ic, /*enumerable=*/true,
                    /*defineOwn=*/true, /*receiver=*/nullptr, &refusal);
                if (refusal == SetRefusal::NotExtensible) {
                    rtThrowTypeError("Cannot define property " +
                                     (name.isSymbol() ? std::string("a symbol")
                                                      : std::string(rtUtf8Chars(name.string()))) +
                                     ", object is not extensible");
                    return false;
                }
                if (refusal != SetRefusal::None) {
                    fatal("internal: a define-form setProp refused for a reason other than extensibility");
                }
                return true;
            }
        }
    }
    return rtDefineDataPropertyComplete(target, name, value);
}

extern "C" {

// `k: v` with a name known at compile time, in an object literal or a class
// field. `icEntry` is the site's own entry (or null): bronze_prop_set's
// entries are never handed here, and this never hands one there.
void bronze_prop_define(uint64_t objBits, uint32_t keyIndex, uint64_t valBits, uint64_t* icEntry) {
    recordPropCall("bronze_prop_define", keyIndex, icEntry);
    Value objVal(objBits);
    InlineCache* ic = rtAsCache(icEntry);
    if (ic && objVal.isObject()) {
        auto* hdr = objVal.asObject<HeapObjectHeader>();
        if (hdr->flags == HeapKind::Plain) {
            auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
            if (obj->shape && ic->describesOwn(obj->shape)) {
                obj->setSlot(ic->cached_slot, Value(valBits));
                return;
            }
        }
    }
    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("property definition with an unregistered key index");
    Rooted<Value> target{objVal};
    Rooted<Value> value{Value(valBits)};
    rtCreateDataPropertyOrThrow(target, PropertyKey::forString(keyHeader), value, ic);
}

// `{ [e]: v }`, a numeric key (`{ 0: v }`), and a computed class field. The
// key is ToPropertyKey'd here (7.1.19, which can run a user `toString`) with
// the receiver and the value rooted across it. No cache: the per-thread
// element-store cache bronze_elem_set keeps is an ASSIGNMENT's and is never
// read here, so a set it remembered cannot be replayed as this define.
void bronze_elem_define(uint64_t objBits, uint64_t keyBits, uint64_t valBits) {
    recordElemCall("bronze_elem_define", objBits, keyBits);
    Rooted<Value> target{Value(objBits)};
    Rooted<Value> value{Value(valBits)};
    Rooted<Value> keyRoot{Value(keyBits)};
    keyRoot.set(rtToPropertyKey(keyRoot));
    const PropertyKey name = rtInternPropertyKey(keyRoot.get());
    rtCreateDataPropertyOrThrow(target, name, value, /*ic=*/nullptr);
}

// `__proto__: v` in an object literal (13.2.5.5 step 6 and B.3.1): not a
// property at all, but the fresh object's [[SetPrototypeOf]] — and only when
// `v` is an object or null; any other value is ignored, not an error. Only
// the plain colon spelling reaches here; `['__proto__']: v`, the shorthand
// `{ __proto__ }` and a method named `__proto__` define an own property.
void bronze_object_literal_proto(uint64_t objBits, uint64_t protoBits) {
    Value protoVal(protoBits);
    if (!protoVal.isObject() && !protoVal.isNull()) return;
    Rooted<Value> obj{Value(objBits)};
    Rooted<Value> proto{protoVal};
    // The literal's object is fresh and ordinary, so 10.1.2.1 cannot refuse
    // (a cycle needs the object to already be reachable from `v`).
    if (!rtObjectSetPrototypeOfOrdinary(obj, proto)) {
        fatal("internal: an object literal's __proto__ was refused");
    }
}

}  // extern "C"

}  // namespace bronze::runtime
