// `super.k` and `super[k]` reads (bronze_super_get, bronze_super_elem_get).
// The walk starts at the home object's prototype and an accessor runs with
// `this` as its receiver; `super.k = v` is a write and lives in
// rt_prop_write.cpp.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

// A static `super` read's statics walk: `key` on the base constructor `base`
// or up its statics chain (rt_property.h rtStaticsAncestor), read off the
// holder that owns it with `receiver` as `this`. False when no link holds it,
// for the function member ladder to answer. Stops at %Object.prototype%,
// whose members are the ladder's last step.
static bool superStaticRead(Rooted<Value>& base, Rooted<Value>& key, Rooted<Value>& receiver,
                            uint64_t& out) {
    const uint64_t objProtoBits = rtObjectPrototype().rawBits();
    const PropertyKey pkey = PropertyKey::fromValue(key.get());
    ObjectHeader* fnObj = base.get().asObject<FunctionHeader>()->object();
    for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        ObjectHeader* holder = depth == 0 ? fnObj : rtStaticsAncestor(fnObj, depth);
        if (!holder || Value::fromObject(holder).rawBits() == objProtoBits) return false;
        PropertyInfo info;
        if (holder->shape && holder->shape->lookupProperty(pkey, info)) {
            out = holder->getProp(rtHeap(), key, /*ic=*/nullptr, receiver.slot_ptr()).rawBits();
            return true;
        }
    }
    return false;
}

extern "C" {

// `super.k` — a read of the PARENT prototype's property, with `this` as the
// receiver (ECMA-262 13.3.7.3, MakeSuperPropertyReference). For a method the
// receiver makes no difference: the value is the same function object either
// way, which is why lowering could spell `super.m` as an ordinary read for as
// long as bronze had no accessors. For a GETTER it is the whole difference —
// running it against the prototype would read the prototype's fields on every
// instance, silently.
uint64_t bronze_super_get(uint64_t protoBits, uint32_t keyIndex, uint64_t thisBits) {
    recordPropCall("bronze_super_get", keyIndex, nullptr);
    Value protoVal(protoBits);
    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("super property read with an unregistered key index");

    if (protoVal.isNull() || protoVal.isUndefined()) {
        return rtThrowTypeError("Cannot read properties of " +
                                std::string(protoVal.isNull() ? "null" : "undefined") +
                                " (reading '" + std::string(rtKeyString(keyIndex)) + "')")
            .rawBits();
    }

    Rooted<Value> receiver{Value(thisBits)};
    Rooted<Value> protoRoot{protoVal};

    if (protoVal.isObject() && protoVal.asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) {
        Rooted<Value> key(Value::fromString(keyHeader));
        return rtProxyGet(protoRoot.get(), key.get(), receiver.get()).rawBits();
    }

    // A STATIC element's `super.k`: the holder is the base CONSTRUCTOR, whose
    // statics are its own and whose inherited statics live up its statics
    // chain (`extends` roots it at the next base). A static found there runs
    // with `this` as its receiver, the same as an instance read below; one
    // found nowhere on the chain is answered by the function member ladder —
    // an intrinsic's statics, `call`/`apply`/`bind`, `name` — on the holder's
    // own terms, which is what `super.of()` in a subclass of Array reaches.
    if (protoVal.isObject() && protoVal.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        Rooted<Value> key(Value::fromString(keyHeader));
        if (uint64_t out = 0; superStaticRead(protoRoot, key, receiver, out)) return out;
        return rtFunctionMember(protoRoot.get(), rtKeyString(keyIndex), keyHeader, nullptr);
    }

    // An exotic home prototype (`{ __proto__: [1, 2], m() { return super[1]; } }`)
    // answers from its own internal method with `this` as the receiver
    // (rt_exotic_link.cpp).
    if (protoVal.isObject() && rtIsExoticLink(protoVal)) {
        Rooted<Value> key(Value::fromString(keyHeader));
        return rtLinkGet(protoRoot, key, receiver).rawBits();
    }
    if (!protoVal.isObject() ||
        !HeapKind::shapeNamesPrototype(protoVal.asObject<HeapObjectHeader>()->flags)) {
        fatal("internal: super property read on a base whose prototype is not an object");
    }
    Rooted<Value> key(Value::fromString(keyHeader));
    // No inline cache: an entry describes ONE shape, and this read has two
    // objects — the holder it walks from and the receiver it runs against.
    return protoRoot.get()
        .asObject<ObjectHeader>()
        ->getProp(rtHeap(), key, /*ic=*/nullptr, receiver.slot_ptr())
        .rawBits();
}

uint64_t bronze_super_elem_get(uint64_t protoBits, uint64_t keyBits, uint64_t thisBits) {
    Value protoVal(protoBits);
    if (protoVal.isNull() || protoVal.isUndefined()) {
        return rtThrowTypeError("Cannot read properties of " +
                                std::string(protoVal.isNull() ? "null" : "undefined"))
            .rawBits();
    }
    Rooted<Value> protoRoot{protoVal};
    Rooted<Value> keyRoot{Value(keyBits)};
    Rooted<Value> receiver{Value(thisBits)};

    keyRoot.set(rtToPropertyKey(keyRoot));

    if (protoVal.isObject() && protoVal.asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) {
        return rtProxyGet(protoRoot.get(), keyRoot.get(), receiver.get()).rawBits();
    }

    if (protoVal.isObject() && protoVal.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        if (uint64_t out = 0; superStaticRead(protoRoot, keyRoot, receiver, out)) return out;
        if (keyRoot.get().isString()) {
            StringHeader* sh = keyRoot.get().asString<StringHeader>();
            return rtFunctionMember(protoRoot.get(), sh, nullptr);
        }
        return Value::fromUndefined().rawBits();
    }

    if (protoRoot.get().isObject() && rtIsExoticLink(protoRoot.get())) {
        return rtLinkGet(protoRoot, keyRoot, receiver).rawBits();
    }
    if (!protoVal.isObject() ||
        !HeapKind::shapeNamesPrototype(protoVal.asObject<HeapObjectHeader>()->flags)) {
        return bronze_elem_get(protoRoot.get().rawBits(), keyRoot.get().rawBits());
    }

    return protoRoot.get()
        .asObject<ObjectHeader>()
        ->getProp(rtHeap(), keyRoot, /*ic=*/nullptr, receiver.slot_ptr())
        .rawBits();
}

}  // extern "C"

}  // namespace bronze::runtime
