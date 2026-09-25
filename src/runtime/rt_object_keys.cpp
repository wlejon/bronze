// Own-key NAMES: the three `*OwnKeyNames` answers a String exotic, an Array and
// an arena-keyed object give, the arena-key-as-value rule they share, and
// `bronze_object_keys` — 20.1.2.17 Object.keys over every receiver kind. Split
// out of rt_object.cpp, which keeps construction, the class links and the
// dynamic call path.

#include <charconv>
#include <string_view>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_object.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/namespace.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/proxy.h"
#include "runtime/regexp.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

// An array index as the STRING that names it — the spelling ToPropertyKey gives
// it, and the only one an own-key answer may contain. `std::to_chars` rather
// than a stream for the reason every number that reaches output uses it:
// deterministic bytes with no locale in the path.
static Value indexName(uint32_t index) {
    char buf[16];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), index);
    return Value::fromString(
        StringHeader::createFromUTF8(rtHeap(), std::string_view(buf, end - buf)));
}

Value rtStringOwnKeyNames(Value strVal, bool enumerableOnly) {
    Rooted<Value> self{strVal};
    Value data = self.get();
    if (!data.isString()) rtStringWrapperData(self.get(), data);
    Rooted<Value> dataRoot{data};
    const uint32_t length = dataRoot.get().asString<StringHeader>()->getLength();
    // 10.4.3.3 step 5: after the indices and `length` come the object's
    // ORDINARY own string keys. A primitive string has none — the characters
    // are the whole story — but a String exotic OBJECT is where the string
    // methods live, and `String.prototype` is one of those objects
    // (22.1.3 makes it a String exotic with [[StringData]] ""). Without this
    // walk `Object.getOwnPropertyNames(String.prototype)` was `["length"]`
    // while `String.prototype.hasOwnProperty("slice")` was true: the keys were
    // in the shape and the listing never looked at it.
    std::vector<StringHeader*> named;
    if (self.get().isObject()) {
        named = rtOwnStringKeysOrdered(self.get().asObject<ObjectHeader>(), enumerableOnly);
    }
    const uint32_t total =
        (enumerableOnly ? length : length + 1) + static_cast<uint32_t>(named.size());
    Rooted<Value> out{Value(bronze_create_array(total))};
    for (uint32_t i = 0; i < length; ++i) {
        Rooted<Value> key{indexName(i)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, key);
    }
    uint32_t at = length;
    if (!enumerableOnly) {
        Rooted<Value> key{rtMakeString("length")};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
    }
    // The keys are arena-interned and immortal, so the vector survives the
    // allocations the copy makes — the same rule the array arm below relies on.
    for (StringHeader* name : named) {
        Rooted<Value> key{rtKeyAsValue(name)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
    }
    return out.get();
}

Value rtArrayOwnKeyNames(Value arrVal, bool enumerableOnly) {
    Rooted<Value> src{arrVal};
    // The indices it actually HAS, already in ascending order: a hole left by
    // `delete a[i]` is not an own property, so the result can be shorter than
    // `length`.
    const uint32_t length = src.get().asObject<ArrayHeader>()->length;
    Rooted<Value> out{Value::fromObject(ArrayHeader::create(rtHeap(), length ? length : 4))};
    uint32_t at = 0;
    for (uint32_t i = 0; i < length; ++i) {
        if (!src.get().asObject<ArrayHeader>()->hasElem(i)) continue;
        Rooted<Value> key{indexName(i)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
    }
    // `length` is where `Object.keys` and `getOwnPropertyNames` part: 10.4.2.2
    // makes it non-enumerable, and it precedes every named property because
    // ArrayCreate defines it before a program can assign one.
    if (!enumerableOnly) {
        Rooted<Value> key{rtMakeString("length")};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
    }
    // Then the named ones — the indices come first because they are
    // integer-like keys, and own-key order puts those ahead of the rest. The
    // keys are arena-interned and immortal, so the vector survives the
    // allocations the copy below makes.
    for (StringHeader* k : rtArrayOwnNamedKeys(src.get(), enumerableOnly)) {
        Rooted<Value> key{rtKeyAsValue(k)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
    }
    return out.get();
}

Value rtKeyAsValue(const StringHeader* key) {
    // The ARENA key itself, not a copy — and that is the mechanism, not a
    // shortcut. Every caller hands this an arena-interned key (a shape key
    // walked by for-in / Object.keys, a module export name, a function
    // static's name): immortal, non-moving, and IMMUTABLE like every string,
    // while a JS string has no observable identity — `===` is content
    // equality — so the aliasing cannot be told apart from the copy this
    // function used to make. What CAN be told apart is the cost: the copy
    // allocated one heap string per key per enumeration (three.js's for-in
    // over `geometry.attributes` made ~5M a run), and it broke object
    // identity, which the computed-read cache's string-key latch
    // (elem_ic.h's `key_ident`) guards on — a fresh copy per frame missed
    // the latch exactly once per site per enumeration, where the arena key
    // hits forever. The collector is indifferent: `forward_value` skips a
    // payload outside the reservation, and the heap verifier admits arena
    // strings in scanned slots by name.
    return Value::fromString(key);
}

extern "C" {

// ECMA-262 20.1.2.17 Object.keys: own ENUMERABLE STRING keys, which is 7.3.23
// EnumerableOwnProperties with key-of-type-String.
//
// Every receiver gets an answer here or is named, and the difference between
// those two is not how much bronze has built — it is whether the answer is
// COMPLETE. A Map, a Set, a RegExp, an ArrayBuffer and a DataView have no own
// enumerable string-keyed property at all, so the empty array is derivable
// rather than a gap dressed up as a result; a typed array's indices ARE own
// enumerable properties (10.4.5.3), so it answers those and not `[]`. The old
// message said which receivers were "supported", which names bronze's coverage
// where the reader needs the receiver's storage.
uint64_t bronze_object_keys(uint64_t objBits) {
    recordHelperCall("bronze_object_keys");
    Value objVal(objBits);
    // Step 1 is ToObject, whose only two failures are these (7.1.18). Thrown
    // rather than fatal: the language names this TypeError, so a `catch` may
    // hold it.
    if (objVal.isNull() || objVal.isUndefined()) {
        return rtThrowTypeError("Object.keys called on a value that is not an object").rawBits();
    }
    // ToObject("ab") is a String exotic object whose own keys are the indices
    // and `length` (10.4.3.3) — and only the indices are enumerable, so those
    // are the answer. Computed from the characters rather than from a box built
    // to be read once and thrown away, which is the arrangement
    // `Object.getPrototypeOf` of a primitive already uses.
    if (objVal.isString()) return rtStringOwnKeyNames(objVal, /*enumerableOnly=*/true).rawBits();
    // The WRAPPER, not its [[StringData]]: 10.4.3.3 lists the object's own
    // ordinary keys after the characters, and handing the primitive over here
    // dropped every expando a program had put on the wrapper.
    if (Value data; rtStringWrapperData(objVal, data)) {
        return rtStringOwnKeyNames(objVal, /*enumerableOnly=*/true).rawBits();
    }
    // A number, a boolean and a symbol box to an object with no own property of
    // any kind, so the empty answer needs no box either — which is what lets
    // `Object.keys(5)` answer at all, since bronze has no Number.prototype for
    // one to point at.
    if (!objVal.isObject()) return bronze_create_array(0);

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();

    // An array's own keys are its indices, already in ascending order — the
    // ones it actually HAS: a hole left by `delete a[i]` is not an own
    // property, so the result is shorter than `length`.
    if (hdr->flags == HeapKind::Array) {
        return rtArrayOwnKeyNames(objVal, /*enumerableOnly=*/true).rawBits();
    }
    // A typed array's integer-indexed elements are own enumerable properties
    // (10.4.5.3 [[DefineOwnProperty]] gives one `enumerable: true`). There are
    // no holes: 23.2.5.1 allocates every element, so the keys are exactly
    // `0..length-1`, and 10.4.5.7 [[OwnPropertyKeys]] lists them ahead of the
    // ordinary string keys the view's shape holds (a subclass field, an
    // expando). `length`, `buffer` and `byteOffset` are accessors on
    // %TypedArray%.prototype and own properties of nothing.
    if (hdr->flags == TypedArrayHeader::kFlags) {
        const uint32_t length = reinterpret_cast<TypedArrayHeader*>(hdr)->length;
        const std::vector<StringHeader*> named =
            rtOwnStringKeysOrdered(reinterpret_cast<ObjectHeader*>(hdr));
        Rooted<Value> out{Value(bronze_create_array(length + static_cast<uint32_t>(named.size())))};
        for (uint32_t i = 0; i < length; ++i) {
            Rooted<Value> key{indexName(i)};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, key);
        }
        uint32_t at = length;
        for (StringHeader* name : named) {
            Rooted<Value> key{rtKeyAsValue(name)};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        return out.get().rawBits();
    }
    // A module namespace: 10.4.6.2's export names, SORTED by code unit, and
    // every one of them enumerable (10.4.6.5), so `Object.keys` and
    // `getOwnPropertyNames` report the same list. The sort happened once at
    // construction — this only copies it out, which is what makes the order a
    // function of the export names rather than of any table walked here.
    if (hdr->flags == ModuleNamespaceHeader::kFlags) {
        Rooted<Value> src{objVal};
        const std::vector<StringHeader*> names = rtModuleNamespaceKeys(src.get());
        Rooted<Value> out{Value(bronze_create_array(static_cast<uint32_t>(names.size())))};
        uint32_t at = 0;
        for (StringHeader* name : names) {
            Rooted<Value> key{rtKeyAsValue(name)};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        return out.get().rawBits();
    }
    // A function's own keys are `length`, `name` and `prototype` — every one of
    // them non-enumerable (10.2.4, 20.2.4) — plus the statics it was assigned,
    // which are the only group this member reports. Those live in the side
    // object, so the answer is COMPLETE, and that is what separates this from
    // `Object.getOwnPropertyNames` of the same function: that member wants the
    // non-enumerable three, and bronze stores none of them.
    if (hdr->flags == HeapKind::Function) {
        Value props = objVal.asObject<FunctionHeader>()->properties;
        if (!props.isObject()) return bronze_create_array(0);
        Rooted<Value> propsRoot{props};
        const std::vector<StringHeader*> named =
            rtOwnStringKeysOrdered(propsRoot.get().asObject<ObjectHeader>());
        Rooted<Value> out{Value(bronze_create_array(static_cast<uint32_t>(named.size())))};
        uint32_t at = 0;
        for (StringHeader* k : named) {
            Rooted<Value> key{rtKeyAsValue(k)};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        return out.get().rawBits();
    }
    // A RegExp takes the ordinary tail: its `lastIndex` is an own property
    // held in the header but NON-ENUMERABLE (22.2.4.1), so the shape's keys —
    // whatever expandos the program wrote — are the complete answer.
    if (hdr->flags == ProxyHeader::kFlags) {
        // 20.1.2.17 is 7.3.23 EnumerableOwnProperties, which on a proxy is
        // [[OwnPropertyKeys]] filtered by [[GetOwnProperty]]'s `enumerable` —
        // the `ownKeys` and `getOwnPropertyDescriptor` traps, in that order.
        Rooted<Value> proxyRoot{objVal};
        // Rooted and re-read per iteration: the descriptor trap below is user
        // code that allocates, so a raw list of key Values would hold
        // from-space strings by the second key.
        Rooted<Value> keys{rtProxyOwnKeys(proxyRoot.get())};
        if (rtExceptionPending()) return bronze_create_array(0);
        Rooted<Value> out{Value(bronze_create_array(0))};
        uint32_t at = 0;
        const uint32_t keyCount = keys.get().asObject<ArrayHeader>()->length;
        for (uint32_t ki = 0; ki < keyCount; ++ki) {
            Rooted<Value> key{keys.get().asObject<ArrayHeader>()->getElem(ki)};
            // 7.3.23 takes the STRING half only; a symbol is never one of the
            // names `Object.keys` reports.
            if (!key.get().isString()) continue;
            OwnPropertyDetail found;
            const bool present = rtProxyGetOwnProperty(proxyRoot.get(), key.get(), found);
            if (rtExceptionPending()) return out.get().rawBits();
            if (!present || !found.enumerable) continue;
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        return out.get().rawBits();
    }
    if (!HeapKind::carriesShape(hdr->flags)) {
        // An iteration record and an environment record are the remainder, and
        // nothing hands a program either — so reaching here is a lowering bug
        // rather than something a program did.
        fatal("internal: Object.keys on an object kind no program can hold");
    }

    // `Object.keys` is own ENUMERABLE STRING keys (20.1.2.17 -> 7.3.23 with
    // key-of-type-String), so the symbol half of the own keys never reaches
    // here — which is how a symbol-keyed property becomes invisible to
    // `Object.keys`, `Object.entries` and `JSON.stringify` at once, by BEING a
    // symbol rather than by any rule about its spelling.
    const std::vector<StringHeader*> ordered =
        rtOwnStringKeysOrdered(reinterpret_cast<ObjectHeader*>(hdr));

    const uint32_t total = static_cast<uint32_t>(ordered.size());
    Rooted<Value> out{Value::fromObject(ArrayHeader::create(rtHeap(), total ? total : 4))};

    uint32_t at = 0;
    for (StringHeader* name : ordered) {
        // Copy the immortal arena string into the heap: the result array holds
        // ordinary JS strings, not pointers into the shape arena.
        Rooted<Value> key{rtKeyAsValue(name)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
    }

    return out.get().rawBits();
}

}  // extern "C"

}  // namespace bronze::runtime
