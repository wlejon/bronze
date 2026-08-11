// Property and element access: the `o.k` and `o[i]` halves of the ABI.
//
// Each receiver kind is its own branch because each stores properties
// differently — an array in its elements, a typed array in its buffer, a
// function in its prototype slot and own-property object, a plain object in
// its shape and slots. A name the receiver's prototype really defines and
// bronze has not built is diagnosed by rt_members.cpp rather than read as
// `undefined`.

#include <cmath>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

// The IC table is a zero-initialized global array in the GENERATED object
// file, one entry per property site, and these helpers take the entry pointer
// (docs/0010 decision 7). That is what lets compiled code hold a stable
// address per site and inline the shape check, which a std::vector — which
// reallocates — could never offer.
//
// `entry` is null only when a caller has no site to cache against (the
// runtime's own property paths); ObjectHeader::getProp already treats a null
// cache as "look it up and cache nothing", a difference in speed and not in
// semantics.
static InlineCache* asCache(uint64_t* entry) noexcept {
    return reinterpret_cast<InlineCache*>(entry);
}

// Whether a key names an ELEMENT rather than a named property, for the
// receivers that store their elements by index. This is `rtIsIntegerLikeKey`
// and nothing else: the canonical-array-index test of docs/0009 decision 1,
// the same one enumeration order and `Object.keys` ask, so the two answers
// cannot drift. A leading-digits parse would send `a["1x"]` and `a["01"]` to
// element 1, which the language calls named properties (`index_keys`).
static bool keyAsIndex(const std::string& key, uint32_t& out) {
    return rtIsIntegerLikeKey(key, out);
}

// A computed index must be a non-negative integral number; anything else on
// the supported receivers reads as undefined / discards the write.
static bool valueToElementIndex(Value idxVal, uint32_t& out) {
    if (!idxVal.isNumber()) {
        fatal("computed index must be a number (string/object keys in [] are unsupported)");
    }
    double d = idxVal.asNumber();
    if (!(d >= 0.0) || d != std::floor(d) || d > 4294967294.0) return false;
    out = static_cast<uint32_t>(d);
    return true;
}

extern "C" {

uint64_t bronze_prop_get(uint64_t objBits, uint32_t keyIndex, uint64_t* icEntry) {
    Value objVal(objBits);
    InlineCache* ic = asCache(icEntry);

    // IC-hit fast path first: a shape match needs no key at all. Generated
    // code inlines the depth-0/inline-slot corner of exactly this check
    // (docs/0010 decision 7) and only calls in when that misses, so what
    // remains hot here is the proto-hit and overflow-slot case.
    if (objVal.isObject()) {
        HeapObjectHeader* fastHdr = objVal.asObject<HeapObjectHeader>();
        if (fastHdr->flags == 0 && ic && ic->cached_shape) {
            auto* fastObj = reinterpret_cast<ObjectHeader*>(fastHdr);
            if (ic->cached_shape == fastObj->shape) {
                // Depth 0 is an own property — the common case, straight to
                // the slot. Anything else was found up the prototype chain, so
                // the cached slot belongs to an ancestor and reading it off the
                // receiver would return an unrelated property (docs/0008
                // decision 2).
                if (ic->cached_depth == 0) {
                    return fastObj->getSlot(ic->cached_slot).rawBits();
                }
                if (ObjectHeader* holder = fastObj->protoAncestor(ic->cached_depth)) {
                    return holder->getSlot(ic->cached_slot).rawBits();
                }
            }
        }
    }

    const std::string& keyStr = rtKeyString(keyIndex);

    if (objVal.isString()) {
        if (keyStr == "length") {
            return Value::fromDouble(objVal.asString<StringHeader>()->getLength()).rawBits();
        }
        Value method = rtStringMethod(keyStr);
        if (!method.isUndefined()) return method.rawBits();
        rtCheckStringMember(keyStr);
        return Value::fromUndefined().rawBits();
    }

    if (!objVal.isObject()) return Value::fromUndefined().rawBits();

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    uint32_t idx = 0;

    if (hdr->flags == 1) {  // Array
        ArrayHeader* arr = reinterpret_cast<ArrayHeader*>(hdr);
        if (keyStr == "length") return Value::fromDouble(arr->length).rawBits();
        if (keyAsIndex(keyStr, idx)) return arr->getElem(idx).rawBits();
        Value method = rtArrayMethod(keyStr);
        if (!method.isUndefined()) return method.rawBits();
        rtCheckArrayMember(keyStr);
        return Value::fromUndefined().rawBits();
    }
    if (hdr->flags == 3) {  // Float32Array view
        auto* view = reinterpret_cast<Float32ArrayHeader*>(hdr);
        if (keyStr == "length") return Value::fromDouble(view->length).rawBits();
        if (keyStr == "buffer") return view->buffer.rawBits();
        if (keyAsIndex(keyStr, idx)) {
            if (idx >= view->length) {
                return Value::fromUndefined().rawBits();
            }
            return Value::fromDouble(static_cast<double>(view->data()[idx])).rawBits();
        }
        rtCheckTypedArrayMember(keyStr);
        return Value::fromUndefined().rawBits();
    }
    if (hdr->flags == 4) {  // ArrayBuffer
        if (keyStr == "byteLength") {
            return Value::fromDouble(reinterpret_cast<ArrayBufferHeader*>(hdr)->byteLength)
                .rawBits();
        }
        rtCheckArrayBufferMember(keyStr);
        return Value::fromUndefined().rawBits();
    }
    if (hdr->flags == 2) {  // Function
        // `prototype` lives in its own slot; every other own property lives in
        // the function's property object and is found through ITS prototype
        // chain, which `extends` linked to the base class's (docs/0012
        // decision 6). Reading `prototype` first is what keeps `call`, `bind`
        // and `name` diagnosed rather than answered as undefined.
        if (keyStr == "prototype") {
            Rooted<Value> fnRoot{objVal};
            rtEnsureFunctionPrototype(fnRoot);
            return fnRoot.get().asObject<FunctionHeader>()->prototype.rawBits();
        }
        Value props = objVal.asObject<FunctionHeader>()->properties;
        if (props.isObject()) {
            Rooted<Value> propsRoot{props};
            Rooted<Value> key(Value::fromString(rtKeyHeader(keyIndex)));
            Value found = propsRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
            if (!found.isUndefined()) return found.rawBits();
        }
        rtCheckFunctionMember(keyStr);
        return Value::fromUndefined().rawBits();
    }

    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("property access with an unregistered key index");
    // Interned arena key: no allocation on the property path.
    Rooted<Value> key(Value::fromString(keyHeader));
    Value result = objVal.asObject<ObjectHeader>()->getProp(rtHeap(), key, ic);
    // A namespace object is an ordinary object, so a member it does not carry
    // reads `undefined` like any other miss — which for a name ECMA-262 says
    // exists is the silent lie rt_members.cpp exists to prevent. Checked only
    // on the miss, so the hit path is untouched.
    if (result.isUndefined()) rtMathCheckMissingMember(objVal, keyStr);
    return result.rawBits();
}

void bronze_prop_set(uint64_t objBits, uint32_t keyIndex, uint64_t valBits, uint64_t* icEntry) {
    Value objVal(objBits);
    Value valVal(valBits);
    InlineCache* ic = asCache(icEntry);
    if (!objVal.isObject()) return;

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();

    // IC-hit fast path: a shape match writes the slot with no key and no
    // rooting (nothing below can allocate). Writes are NOT inlined into
    // generated code: a write can transition the shape and grow the overflow
    // block, so the interesting half of the work is the miss, and the miss is
    // a call either way (docs/0010 decision 7 inlines the read).
    if (hdr->flags == 0 && ic && ic->cached_shape) {
        auto* fastObj = reinterpret_cast<ObjectHeader*>(hdr);
        if (ic->cached_shape == fastObj->shape) {
            fastObj->setSlot(ic->cached_slot, valVal);
            return;
        }
    }

    const std::string& keyStr = rtKeyString(keyIndex);
    uint32_t idx = 0;

    if (hdr->flags == 1) {  // Array
        // Numeric keys store an element. A named write is diagnosed rather
        // than discarded: JS would create the property, and arrays carry no
        // shape for named properties yet.
        if (!keyAsIndex(keyStr, idx)) {
            fatal("named property writes on an array are unsupported "
                  "(arrays carry no shape for named properties yet)");
        }
        Rooted<Value> val(valVal);
        reinterpret_cast<ArrayHeader*>(hdr)->setElem(rtHeap(), idx, val);
        return;
    }
    if (hdr->flags == 3) {  // Float32Array view
        auto* view = reinterpret_cast<Float32ArrayHeader*>(hdr);
        if (!keyAsIndex(keyStr, idx)) {
            fatal("named property writes on a Float32Array are unsupported");
        }
        if (idx < view->length) {
            view->data()[idx] = static_cast<float>(bronze_unbox_f64(valBits));
        }
        return;  // out-of-bounds typed-array writes are discarded, per spec
    }
    if (hdr->flags == 4) {
        fatal("property writes on an ArrayBuffer are unsupported");
    }
    if (hdr->flags == 2) {  // Function
        if (keyStr != "prototype") {
            // A static member: an own property of the function object itself
            // (docs/0012 decision 6).
            Rooted<Value> fnRoot{objVal};
            Rooted<Value> val{valVal};
            rtEnsureFunctionProperties(fnRoot);
            Rooted<Value> propsRoot{fnRoot.get().asObject<FunctionHeader>()->properties};
            Rooted<Value> key(Value::fromString(rtKeyHeader(keyIndex)));
            propsRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
            return;
        }
        if (!valVal.isObject()) {
            fatal("assigning a non-object to a function's `prototype` is unsupported");
        }
        auto* fn = reinterpret_cast<FunctionHeader*>(hdr);
        fn->prototype = valVal;
        // Instances made from here on get the new prototype; ones already made
        // keep their shape, and so keep the old one.
        fn->instance_shape = rtNewRootShape(valVal);
        return;
    }

    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("property write with an unregistered key index");
    // Interned arena key: no allocation before the object is dereferenced.
    // setProp itself may still allocate (overflow growth); it re-derives the
    // object through its own root, and this caller's raw objBits is dead after
    // the call, so that is safe.
    Rooted<Value> key(Value::fromString(keyHeader));
    Rooted<Value> val(valVal);
    objVal.asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, ic);
}

uint64_t bronze_elem_get(uint64_t objBits, uint64_t idxBits) {
    Value objVal(objBits);
    if (!objVal.isObject()) {
        fatal("computed index access on a non-object value is unsupported");
    }
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    uint32_t idx = 0;
    if (hdr->flags == 1) {
        if (!valueToElementIndex(Value(idxBits), idx)) {
            return Value::fromUndefined().rawBits();
        }
        return reinterpret_cast<ArrayHeader*>(hdr)->getElem(idx).rawBits();
    }
    if (hdr->flags == 3) {
        auto* view = reinterpret_cast<Float32ArrayHeader*>(hdr);
        if (!valueToElementIndex(Value(idxBits), idx) || idx >= view->length) {
            return Value::fromUndefined().rawBits();
        }
        return Value::fromDouble(static_cast<double>(view->data()[idx])).rawBits();
    }
    fatal("computed index access is only supported on arrays and Float32Array");
}

void bronze_elem_set(uint64_t objBits, uint64_t idxBits, uint64_t valBits) {
    Value objVal(objBits);
    if (!objVal.isObject()) {
        fatal("computed index write on a non-object value is unsupported");
    }
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    uint32_t idx = 0;
    if (hdr->flags == 1) {
        if (!valueToElementIndex(Value(idxBits), idx)) {
            fatal("non-integer array index write is unsupported");
        }
        Rooted<Value> val{Value(valBits)};
        reinterpret_cast<ArrayHeader*>(hdr)->setElem(rtHeap(), idx, val);
        return;
    }
    if (hdr->flags == 3) {
        auto* view = reinterpret_cast<Float32ArrayHeader*>(hdr);
        if (valueToElementIndex(Value(idxBits), idx) && idx < view->length) {
            view->data()[idx] = static_cast<float>(bronze_unbox_f64(valBits));
        }
        return;  // out-of-bounds typed-array writes are discarded, per spec
    }
    fatal("computed index writes are only supported on arrays and Float32Array");
}

}  // extern "C"

}  // namespace bronze::runtime
