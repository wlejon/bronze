// An EXOTIC OBJECT AS A PROTOTYPE: an array, a typed array, a String wrapper,
// a function or a module namespace on an object's chain.
//
// The ordinary walks (ObjectHeader::getProp and setProp, `in`'s
// plainObjectHas, for-in) step from link to link reading each link's shape,
// which is the whole of an ordinary object's storage. These kinds keep
// members a shape does not hold: an array its elements and `length`, a typed
// array its elements (every numeric key, 10.4.5), a String wrapper its
// string's indices and `length` (10.4.3), a function its `name`, `length` and
// `prototype` and statics up a chain of their own, a namespace its exports.
// So a walk that reaches one hands the rest of the operation to that link's
// own internal method with the walk's receiver, as it does at a proxy
// (10.1.8.1 step 3, 10.1.7.1 step 3, 10.1.9.2 step 1.c): the functions below,
// which answer an operation from a link up to the chain's end over the
// generic own-property query (rtOwnPropertyOf), whatever kind each link is.
//
// What lets every fast path stay as it was: putting any of these on a chain
// trips the array-index hazard (runtime/object.cpp; Shape::createRoot for an
// array, a namespace or a proxy, noteObjectBecomingPrototype for a typed
// array or a String wrapper), and no cache fill crosses one —
// cachedProtoHolder and chainIsCacheable refuse a link that is no ordinary
// object, and once the hazard is set also a typed array or a String wrapper,
// the two such kinds that do carry a shape. The walks below run no cache.

#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_object.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/heap.h"
#include "runtime/integrity.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/tls_block.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool nonEmptyStringWrapper(Value v) {
    Value data;
    return rtStringWrapperData(v, data) && data.isString() && data.asString<StringHeader>()->getLength() > 0;
}

// A typed array's numeric key (10.4.5.4 step 1.b, CanonicalNumericIndexString):
// [[Get]], [[HasProperty]] and [[Set]] answer it from the elements and never
// go on up the chain.
bool typedArrayNumericKey(Value obj, Value key) {
    if (obj.asObject<HeapObjectHeader>()->flags != TypedArrayHeader::kFlags) return false;
    if (key.isNumber()) return true;
    return key.isString() && rtIsCanonicalNumericString(rtUtf8Chars(key.asString<StringHeader>()));
}

// The next link: 10.1.1 [[GetPrototypeOf]] of any kind but a proxy (whose
// trap the callers take before asking).
Value prototypeOf(Value obj) {
    const uint64_t call[1] = {obj.rawBits()};
    return Value(objectGetPrototypeOf(0, 0, 1, call));
}

// Is this link answered by its shape alone, so the ordinary walk can take
// the rest of the operation from it?
bool ordinaryLink(Value link) {
    const uint16_t kind = link.asObject<HeapObjectHeader>()->flags;
    return HeapKind::shapeNamesPrototype(kind) && kind != TypedArrayHeader::kFlags && !nonEmptyStringWrapper(link);
}

}  // namespace

bool rtAnswersOffShape(Value obj) noexcept {
    if (!obj.isObject()) return false;
    switch (obj.asObject<HeapObjectHeader>()->flags) {
        case HeapKind::Array:
        case HeapKind::Function:
        case HeapKind::TypedArray:
        case HeapKind::ModuleNamespace:
            return true;
        case HeapKind::Plain:
            return nonEmptyStringWrapper(obj);
        default:
            return false;
    }
}

bool rtIsExoticLinkSlow(Value link) noexcept {
    // Only a link of a Plain kind reaches here (rt_property.h): a String
    // wrapper, which can be on a chain only once it was made a prototype,
    // and that tripped the hazard.
    return rtTls()->array_index_hazard != 0 && nonEmptyStringWrapper(link);
}

Value rtLinkGet(Rooted<Value>& link, Rooted<Value>& key, Rooted<Value>& receiver) {
    Rooted<Value> holder{link.get()};
    for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        if (!holder.get().isObject()) return Value::fromUndefined();
        const uint16_t kind = holder.get().asObject<HeapObjectHeader>()->flags;
        if (kind == HeapKind::Proxy) return rtProxyGet(holder.get(), key.get(), receiver.get());
        if (ordinaryLink(holder.get())) {
            // The shape walk takes a string or a symbol: an index from
            // `Reflect.get` or `super[i]` is spelled as its string.
            if (key.get().isNumber()) key.set(rtToStringValue(key));
            // The shape walk from here on, which hands off again at the next
            // exotic link. No cache: this read has two objects.
            return holder.get().asObject<ObjectHeader>()->getProp(rtHeap(), key, /*ic=*/nullptr,
                                                                    receiver.slot_ptr());
        }
        if (typedArrayNumericKey(holder.get(), key.get())) {
            return Value(bronze_elem_get(holder.get().rawBits(), key.get().rawBits()));
        }
        OwnPropertyDetail own;
        if (rtOwnPropertyOf(holder, key.get(), own)) {
            if (!own.accessor) {
                // A value the query could not produce without allocating (a
                // string's character, a function's `name`) is the holder's
                // own read, which a receiver cannot change.
                return own.valueKnown ? own.value
                                      : Value(bronze_elem_get(holder.get().rawBits(), key.get().rawBits()));
            }
            if (!rtIsCallableValue(own.getter)) return Value::fromUndefined();
            Rooted<Value> getter{own.getter};
            return Value(bronze_dynamic_call(getter.get().rawBits(), receiver.get().rawBits(), 0, nullptr));
        }
        holder.set(prototypeOf(holder.get()));
    }
    rtThrowTypeError("prototype chain too deep (a cycle?)");
}

bool rtLinkHas(Rooted<Value>& link, Rooted<Value>& key) {
    Rooted<Value> holder{link.get()};
    for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        if (!holder.get().isObject()) return false;
        const uint16_t kind = holder.get().asObject<HeapObjectHeader>()->flags;
        if (kind == HeapKind::Proxy) return rtProxyHas(holder.get(), key.get());
        if (ordinaryLink(holder.get()) || typedArrayNumericKey(holder.get(), key.get())) {
            // `in` from here: the shape walk, or a view's own answer.
            return bronze_has_property(key.get().rawBits(), holder.get().rawBits());
        }
        bool enumerable = false;
        if (rtOwnPropertyOf(holder, key.get(), enumerable)) return true;
        holder.set(prototypeOf(holder.get()));
    }
    rtThrowTypeError("prototype chain too deep (a cycle?)");
}

SetRefusal rtLinkSet(Rooted<Value>& link, Rooted<Value>& key, Rooted<Value>& val, Rooted<Value>& receiver,
                     bool& addOwn) {
    addOwn = false;
    Rooted<Value> holder{link.get()};
    for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        if (!holder.get().isObject()) break;
        const uint16_t kind = holder.get().asObject<HeapObjectHeader>()->flags;
        if (kind == HeapKind::Proxy) {
            // 10.5.9: the trap, or its forward, which defines on the receiver.
            const bool ok = rtProxySet(holder.get(), key.get(), val.get(), /*strict=*/false, receiver.get());
            return ok ? SetRefusal::None : SetRefusal::TrapRefused;
        }
        if (typedArrayNumericKey(holder.get(), key.get())) {
            // 10.4.5.5 step 1.b.ii: an invalid index is a successful no-op; a
            // valid one is a writable data property, so the write lands on
            // the receiver.
            uint32_t index = 0;
            const bool valid = rtValueToElementIndex(key.get(), index) &&
                               index < holder.get().asObject<TypedArrayHeader>()->length;
            if (valid) addOwn = true;
            return SetRefusal::None;
        }
        OwnPropertyDetail own;
        if (rtOwnPropertyOf(holder, key.get(), own)) {
            if (own.accessor) {
                // Step 3: the setter, with the receiver as `this`.
                if (!rtIsCallableValue(own.setter)) return SetRefusal::NoSetter;
                Rooted<Value> setter{own.setter};
                const uint64_t argv[1] = {val.get().rawBits()};
                (void)bronze_dynamic_call(setter.get().rawBits(), receiver.get().rawBits(), 1, argv);
                return SetRefusal::None;
            }
            // Step 2.a, and the override-mistake fix of a hardened intrinsic
            // (lockdown.h), whose own property refuses only itself.
            if (!own.writable && !rtIsHardened(holder.get())) return SetRefusal::NotWritable;
            addOwn = true;
            return SetRefusal::None;
        }
        holder.set(prototypeOf(holder.get()));
    }
    // Step 1.d: absent everywhere, so a new own property of the receiver.
    addOwn = true;
    return SetRefusal::None;
}

}  // namespace bronze::runtime
