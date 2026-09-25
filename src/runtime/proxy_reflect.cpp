// The Proxy exotic object's internal methods whose FORWARD is an `Object`
// member rather than a property read — [[GetOwnProperty]] as a descriptor
// object (10.5.5), [[DefineOwnProperty]] (10.5.6), [[SetPrototypeOf]] (10.5.2),
// [[IsExtensible]] (10.5.3) and [[PreventExtensions]] (10.5.4) — and the two
// integrity algorithms (7.3.14, 7.3.15) and 7.3.23's enumeration, which the
// specification defines over those methods and which therefore reach a
// handler's traps in the order it states.
//
// The two rules of proxy.cpp hold here unchanged: a trap is found with
// GetMethod, and every operation is a GC point and a throw point, so every
// value a step still needs is rooted before the call, and a throw leaves the
// step before anything uses its result.
//
// The forwards go through the `Object` members — `getOwnPropertyDescriptor`,
// `defineProperty`, `setPrototypeOf`, `preventExtensions` — rather than
// through the storage those members are built over, because a target can be
// any kind the members already answer for, a proxy over a proxy included; a
// forward that reached into a shape would answer wrongly for every other kind.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_object.h"
#include "runtime/builtin_object_descriptor_internal.h"
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
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

using descriptor_internal::DecodedDescriptor;
using descriptor_internal::decodeDescriptor;
using descriptor_internal::putField;

namespace {

// 7.3.11 GetMethod(handler, name), on the same terms as proxy.cpp's: undefined
// means "no trap, forward", and a non-callable answer is the TypeError.
Value trapOf(Rooted<Value>& handlerRoot, const char* name) {
    Rooted<Value> key{rtMakeString(name)};
    Value found = Value(bronze_elem_get(handlerRoot.get().rawBits(), key.get().rawBits()));
    if (found.isUndefined() || found.isNull()) return Value::fromUndefined();
    if (!rtIsCallableValue(found)) {
        rtThrowTypeError(std::string("'") + name + "' trap on proxy is not a function");
        return Value::fromUndefined();
    }
    return found;
}

// The revoked check and the two halves, rooted. False means a TypeError is
// pending.
bool openProxy(Value proxyVal, const char* operation, Rooted<Value>& targetRoot,
               Rooted<Value>& handlerRoot) {
    if (rtProxyRefuseIfRevoked(proxyVal, operation)) return false;
    targetRoot.set(proxyVal.asObject<ProxyHeader>()->target);
    handlerRoot.set(proxyVal.asObject<ProxyHeader>()->handler);
    return true;
}

bool isProxy(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == ProxyHeader::kFlags;
}

// A key as text for a diagnostic: `Symbol(desc)` for a symbol (20.4.3.3.1).
std::string keyText(Rooted<Value>& key) {
    if (key.get().isSymbol()) return rtSymbolDescriptiveString(key.get());
    Rooted<Value> str{rtValueToString(key.get())};
    return rtUtf8Chars(str.get().asString<StringHeader>());
}

// 6.2.6.4 FromPropertyDescriptor over a COMPLETED detail — the four fields of
// its kind, in the specification's order, which `Object.keys(descriptor)`
// prints and so is pinned bytes.
Value descriptorFromDetail(const OwnPropertyDetail& d) {
    Rooted<Value> a{d.accessor ? d.getter : d.value};
    Rooted<Value> b{d.accessor ? d.setter : Value::fromUndefined()};
    Rooted<Value> out{Value(bronze_create_object())};
    if (d.accessor) {
        putField(out, "get", a);
        putField(out, "set", b);
    } else {
        putField(out, "value", a);
        Rooted<Value> w{Value::fromBool(d.writable)};
        putField(out, "writable", w);
    }
    Rooted<Value> e{Value::fromBool(d.enumerable)};
    putField(out, "enumerable", e);
    Rooted<Value> c{Value::fromBool(d.configurable)};
    putField(out, "configurable", c);
    return out.get();
}

// A one- or two-field descriptor object, for the defines 7.3.14 makes:
// `{ configurable: false }` and `{ configurable: false, writable: false }`.
Value lockingDescriptor(bool alsoNonWritable) {
    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> f{Value::fromBool(false)};
    putField(out, "configurable", f);
    if (alsoNonWritable) putField(out, "writable", f);
    return out.get();
}

}  // namespace

Value rtProxyGetOwnPropertyDescriptor(Value proxyVal, Value keyVal) {
    Rooted<Value> proxyRoot{proxyVal};
    Rooted<Value> keyRoot{keyVal};
    OwnPropertyDetail found;
    switch (rtProxyGetOwnPropertyTrapped(proxyVal, keyVal, found)) {
        case ProxyOwnProperty::Absent:
            return Value::fromUndefined();
        case ProxyOwnProperty::Present:
            // Step 18: FromPropertyDescriptor of the trap's descriptor after
            // steps 12-13 completed it — a fresh object, not the trap's own.
            return descriptorFromDetail(found);
        case ProxyOwnProperty::Forwarded:
            break;
    }
    // Step 8: the target's own [[GetOwnProperty]], as the member spells it,
    // which describes every kind a target can be.
    Rooted<Value> target{proxyRoot.get().asObject<ProxyHeader>()->target};
    const uint64_t call[2] = {target.get().rawBits(), keyRoot.get().rawBits()};
    return Value(rtObjectGetOwnPropertyDescriptor(0, 0, 2, call));
}

bool rtProxyDefineOwnProperty(Value proxyVal, Value keyVal, Value descVal, bool throwOnRefusal) {
    Rooted<Value> proxyRoot{proxyVal};
    Rooted<Value> keyRoot{keyVal};
    Rooted<Value> descRoot{descVal};
    Rooted<Value> targetRoot;
    Rooted<Value> handlerRoot;
    if (!openProxy(proxyVal, "defineProperty", targetRoot, handlerRoot)) return false;

    Value trap = trapOf(handlerRoot, "defineProperty");
    if (trap.isUndefined()) {
        // Step 7: the target's own [[DefineOwnProperty]], through the member
        // that decodes and applies it for every kind — the descriptor object
        // here is the six-field copy the caller built, so decoding it again
        // runs no program code.
        const uint64_t call[3] = {targetRoot.get().rawBits(), keyRoot.get().rawBits(),
                                  descRoot.get().rawBits()};
        return rtObjectDefineOwnProperty(3, call, throwOnRefusal);
    }
    Rooted<Value> trapRoot{trap};
    const uint64_t args[3] = {targetRoot.get().rawBits(), keyRoot.get().rawBits(),
                              descRoot.get().rawBits()};
    const uint64_t result = bronze_dynamic_call(trapRoot.get().rawBits(),
                                                handlerRoot.get().rawBits(), 3, args);
    // Step 10: a false from the trap is the refusal — 20.1.2.4's TypeError or
    // 28.1.3's `false`, which is the caller's choice and not this method's.
    if (!bronze_truthy(result)) {
        if (throwOnRefusal) {
            rtThrowTypeError("'defineProperty' on proxy: trap returned falsish for property '" +
                             keyText(keyRoot) + "'");
        }
        return false;
    }
    // Steps 11-17 read the descriptor the trap was handed, field by field.
    DecodedDescriptor d;
    Rooted<Value> value{Value::fromUndefined()};
    Rooted<Value> getter{Value::fromUndefined()};
    Rooted<Value> setter{Value::fromUndefined()};
    if (!decodeDescriptor(descRoot, d, value, getter, setter)) return false;
    DecodedDescriptorView view;
    view.hasValue = d.hasValue;
    view.hasWritable = d.hasWritable;
    view.hasEnumerable = d.hasEnumerable;
    view.hasConfigurable = d.hasConfigurable;
    view.hasGet = d.hasGet;
    view.hasSet = d.hasSet;
    view.writable = d.wantWritable;
    view.enumerable = d.wantEnumerable;
    view.configurable = d.wantConfigurable;
    view.value = &value;
    view.getter = &getter;
    view.setter = &setter;
    rtProxyCheckDefineProperty(targetRoot, keyRoot, view);
    return true;
}

bool rtProxySetPrototypeOf(Value proxyVal, Value protoVal) {
    Rooted<Value> proxyRoot{proxyVal};
    Rooted<Value> protoRoot{protoVal};
    Rooted<Value> targetRoot;
    Rooted<Value> handlerRoot;
    if (!openProxy(proxyVal, "setPrototypeOf", targetRoot, handlerRoot)) return false;

    Value trap = trapOf(handlerRoot, "setPrototypeOf");
    if (trap.isUndefined()) {
        // Step 6: the target's own [[SetPrototypeOf]], answering the boolean
        // 10.1.2 answers — which the member turns into a TypeError only for
        // `Object.setPrototypeOf`, and this is not that.
        return rtObjectSetPrototypeOfOrdinary(targetRoot, protoRoot);
    }
    Rooted<Value> trapRoot{trap};
    const uint64_t args[2] = {targetRoot.get().rawBits(), protoRoot.get().rawBits()};
    const bool answer = bronze_truthy(bronze_dynamic_call(trapRoot.get().rawBits(),
                                                          handlerRoot.get().rawBits(), 2, args));
    // Step 9: a refusal contradicts nothing.
    if (!answer) return false;
    // Steps 10-13: an extensible target's prototype can still change, so the
    // trap is free; a non-extensible one's is fixed, and the trap must have
    // told the truth about it.
    const bool extensible = rtIsExtensibleOf(targetRoot);
    if (extensible) return true;
    const uint64_t call[1] = {targetRoot.get().rawBits()};
    const Value actual = Value(objectGetPrototypeOf(0, 0, 1, call));
    if (!sameValue(protoRoot.get(), actual)) {
        rtThrowTypeError(
            "'setPrototypeOf' on proxy: trap returned truish for setting a new prototype on "
            "the non-extensible proxy target");
        return false;
    }
    return true;
}

bool rtProxyIsExtensible(Value proxyVal) {
    Rooted<Value> targetRoot;
    Rooted<Value> handlerRoot;
    if (!openProxy(proxyVal, "isExtensible", targetRoot, handlerRoot)) return false;

    Value trap = trapOf(handlerRoot, "isExtensible");
    if (trap.isUndefined()) return rtIsExtensibleOf(targetRoot);  // step 6
    Rooted<Value> trapRoot{trap};
    const uint64_t args[1] = {targetRoot.get().rawBits()};
    const bool answer = bronze_truthy(bronze_dynamic_call(trapRoot.get().rawBits(),
                                                          handlerRoot.get().rawBits(), 1, args));
    // Steps 8-9: the one trap whose answer must EQUAL the target's, in both
    // directions — extensibility is not something a proxy may lie about
    // either way.
    const bool actual = rtIsExtensibleOf(targetRoot);
    if (answer != actual) {
        rtThrowTypeError(std::string("'isExtensible' on proxy: trap result does not reflect "
                                     "extensibility of proxy target (which is '") +
                         (actual ? "true" : "false") + "')");
        return false;
    }
    return answer;
}

bool rtProxyPreventExtensions(Value proxyVal) {
    Rooted<Value> proxyRoot{proxyVal};
    Rooted<Value> targetRoot;
    Rooted<Value> handlerRoot;
    if (!openProxy(proxyVal, "preventExtensions", targetRoot, handlerRoot)) return false;

    Value trap = trapOf(handlerRoot, "preventExtensions");
    if (trap.isUndefined()) {
        // Step 6: the target's own [[PreventExtensions]]. A proxy target
        // answers its boolean; every other kind the member handles answers
        // true or raises, and the raise is the answer.
        if (isProxy(targetRoot.get())) return rtProxyPreventExtensions(targetRoot.get());
        const uint64_t call[1] = {targetRoot.get().rawBits()};
        rtObjectPreventExtensions(0, 0, 1, call);
        return true;
    }
    Rooted<Value> trapRoot{trap};
    const uint64_t args[1] = {targetRoot.get().rawBits()};
    const bool answer = bronze_truthy(bronze_dynamic_call(trapRoot.get().rawBits(),
                                                          handlerRoot.get().rawBits(), 1, args));
    // Step 8: a trap may claim success only once the target really can gain
    // nothing — otherwise the proxy would report a closed object over an
    // open one.
    if (answer) {
        const bool extensible = rtIsExtensibleOf(targetRoot);
        if (extensible) {
            rtThrowTypeError("'preventExtensions' on proxy: trap returned truish but the proxy "
                             "target is extensible");
            return false;
        }
    }
    return answer;
}

bool rtProxySetIntegrityLevel(Value proxyVal, IntegrityLevel level) {
    Rooted<Value> proxyRoot{proxyVal};
    // 7.3.14 step 3 (and the whole of 20.1.2.19): [[PreventExtensions]] first,
    // and a false ends the operation as a false.
    if (!rtProxyPreventExtensions(proxyRoot.get())) return false;
    if (level == IntegrityLevel::Open) return true;
    // Step 5: [[OwnPropertyKeys]] once, then a DefinePropertyOrThrow per key —
    // `{ configurable: false }` to seal, and to freeze the same plus
    // `writable: false` for a DATA property, which is why the frozen loop
    // asks [[GetOwnProperty]] first (step 7.b.i) and the sealed one does not.
    Rooted<Value> keys{rtProxyOwnKeys(proxyRoot.get())};
    const uint32_t count = keys.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> key{keys.get().asObject<ArrayHeader>()->getElem(i)};
        bool lockWritable = false;
        if (level == IntegrityLevel::Frozen) {
            OwnPropertyDetail current;
            const bool exists = rtProxyGetOwnProperty(proxyRoot.get(), key.get(), current);
            // 7.3.14 step 7.b.ii: a key the proxy no longer reports — its
            // `ownKeys` and `getOwnPropertyDescriptor` traps may disagree —
            // is skipped, not defined.
            if (!exists) continue;
            lockWritable = !current.accessor;
        }
        Rooted<Value> desc{lockingDescriptor(lockWritable)};
        if (!rtProxyDefineOwnProperty(proxyRoot.get(), key.get(), desc.get(),
                                      /*throwOnRefusal=*/true)) {
            return false;
        }
    }
    return true;
}

bool rtProxyTestIntegrityLevel(Value proxyVal, bool frozen) {
    Rooted<Value> proxyRoot{proxyVal};
    // 7.3.15 step 3: an extensible object is neither sealed nor frozen, and
    // the keys are never asked for.
    const bool extensible = rtProxyIsExtensible(proxyRoot.get());
    if (extensible) return false;
    // Steps 5-7: every own key's descriptor, stopping at the first one that
    // answers the question — a configurable property, or for the frozen
    // question a writable data property.
    Rooted<Value> keys{rtProxyOwnKeys(proxyRoot.get())};
    const uint32_t count = keys.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> key{keys.get().asObject<ArrayHeader>()->getElem(i)};
        OwnPropertyDetail current;
        const bool exists = rtProxyGetOwnProperty(proxyRoot.get(), key.get(), current);
        if (!exists) continue;
        if (current.configurable) return false;
        if (frozen && !current.accessor && current.writable) return false;
    }
    return true;
}

bool rtIsArray(Value v) {
    for (uint32_t depth = 0; depth <= 1000; ++depth) {
        if (!v.isObject()) return false;
        const uint16_t kind = v.asObject<HeapObjectHeader>()->flags;
        if (kind == HeapKind::Array) return true;
        if (rtIsArrayPrototypeObject(v)) return true;  // 23.1.3: an Array exotic object
        if (kind != ProxyHeader::kFlags) return false;
        // Step 3: through the proxy to its target, and a revoked one throws
        // rather than answering — 'IsArray' is the operation 7.2.2 names.
        if (rtProxyRefuseIfRevoked(v, "IsArray")) return false;
        v = v.asObject<ProxyHeader>()->target;
    }
    fatal("proxy target chain too deep (a cycle?)");
}

Value rtProxyEnumerableOwn(Value proxyVal, bool wantEntries) {
    Rooted<Value> proxyRoot{proxyVal};
    Rooted<Value> keys{rtProxyOwnKeys(proxyRoot.get())};
    Rooted<Value> out{Value(bronze_create_array(0))};
    const uint32_t count = keys.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> key{keys.get().asObject<ArrayHeader>()->getElem(i)};
        // 7.3.23 step 4.a: the string keys only; a symbol is never one of the
        // names `Object.values` or `Object.entries` reports.
        if (!key.get().isString()) continue;
        OwnPropertyDetail found;
        const bool present = rtProxyGetOwnProperty(proxyRoot.get(), key.get(), found);
        if (!present || !found.enumerable) continue;
        // Step 4.a.ii.2: the [[Get]] follows THIS key's descriptor, before the
        // next key's is asked for.
        Rooted<Value> val{rtProxyGet(proxyRoot.get(), key.get())};
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
    return out.get();
}

}  // namespace bronze::runtime
