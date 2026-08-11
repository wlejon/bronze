// Property and element access: the `o.k` and `o[i]` halves of the ABI.
//
// Each receiver kind is its own branch because each stores properties
// differently — an array in its elements, a typed array in its buffer, a
// function in its prototype slot and own-property object, a plain object in
// its shape and slots. A name the receiver's prototype really defines and
// bronze has not built is diagnosed by rt_members.cpp rather than read as
// `undefined`.

#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/number_format.h"
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

// A computed index that names an element. ToPropertyKey makes `a[0]` and
// `a["0"]` the same property, so a STRING index that is a canonical array
// index has to reach the elements too — which is not an edge case: `for-in`
// yields keys as strings, so `arr[k]` inside one is the ordinary way to write
// this. Before the string branch existed, `for (const i in arr) arr[i]` died
// with "computed index must be a number" on the loop's own idiom.
//
// A non-canonical string ("01", "1x") is a NAMED property, which arrays and
// typed arrays do not carry; the caller answers `undefined` for it, exactly as
// for an out-of-range index.
static bool valueToElementIndex(Value idxVal, uint32_t& out) {
    if (idxVal.isString()) {
        const StringHeader* s = idxVal.asString<StringHeader>();
        if (!s->isLatin1()) return false;
        return rtIsIntegerLikeKey(std::string_view(s->latin1Data(), s->getLength()), out);
    }
    if (!idxVal.isNumber()) {
        fatal("computed index must be a number or a string (object keys in [] are unsupported)");
    }
    double d = idxVal.asNumber();
    if (!(d >= 0.0) || d != std::floor(d) || d > 4294967294.0) return false;
    out = static_cast<uint32_t>(d);
    return true;
}

// ToPropertyKey (ECMA-262 7.1.19) as a heap string: every property name is a
// string, so `o[2]` and `o["2"]` name the same property and `{ [2]: v }` and
// `{ 2: v }` write the same one. ToString(Number) is `formatJsNumber` and not
// console.log's inspect spelling — ToString(-0) is "0", where inspect says
// "-0" (docs/0013 decision 1).
//
// ALLOCATES, so the caller must have the receiver rooted before it calls.
static Value elemKeyAsString(Value idxVal) {
    if (idxVal.isString()) return idxVal;
    char buf[64];
    size_t len = 0;
    if (idxVal.isNumber()) {
        len = formatJsNumber(idxVal.asNumber(), buf);
    } else if (idxVal.isBool()) {
        len = idxVal.asBool() ? 4 : 5;
        std::memcpy(buf, idxVal.asBool() ? "true" : "false", len);
    } else if (idxVal.isUndefined()) {
        len = 9;
        std::memcpy(buf, "undefined", len);
    } else if (idxVal.isNull()) {
        len = 4;
        std::memcpy(buf, "null", len);
    } else {
        // An object key would need ToPrimitive, which docs/0015 decision 7
        // names as the same missing piece behind `String(obj)` and `==`
        // between an object and a primitive.
        fatal("a computed property key that is an object needs ToPrimitive, "
              "which is unsupported");
    }
    return Value::fromString(
        StringHeader::createFromUTF8(rtHeap(), std::string_view(buf, len)));
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
                // An ancestor's slot numbering is stable while it is a
                // transition-tree shape and not once it is a dictionary,
                // which reuses freed slots for unrelated names — and the
                // receiver's shape, which is all this entry checks, does not
                // change when its PROTOTYPE is deleted from (docs/0019
                // decision 5).
                if (ObjectHeader* holder = fastObj->protoAncestor(ic->cached_depth)) {
                    if (!holder->shape || !holder->shape->isDictionary()) {
                        return holder->getSlot(ic->cached_slot).rawBits();
                    }
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

    // Reading a property of null or undefined is a TypeError in ECMA-262
    // 7.3.2 (GetV -> ToObject), and answering `undefined` for it is the
    // silent-wrong-answer shape CLAUDE.md forbids: `a.b.c` where `a.b` is
    // missing would report nothing and carry an undefined onward. It is also
    // what makes `(a?.b).c` differ observably from `a?.b.c` (docs/0018
    // decision 4). Catchable since docs/0020: the spec names it, so it is a
    // thrown TypeError rather than the process death it used to be.
    if (objVal.isNull() || objVal.isUndefined()) {
        return rtThrowTypeError("Cannot read properties of " +
                                std::string(objVal.isNull() ? "null" : "undefined") +
                                " (reading '" + keyStr + "')")
            .rawBits();
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
            // The receiver a `static get` sees is the CLASS, not the side
            // object its statics are kept in — which is the whole reason
            // getProp takes a receiver at all.
            Rooted<Value> fnRoot{objVal};
            Rooted<Value> propsRoot{props};
            Rooted<Value> key(Value::fromString(rtKeyHeader(keyIndex)));
            Value found = propsRoot.get().asObject<ObjectHeader>()->getProp(
                rtHeap(), key, /*ic=*/nullptr, fnRoot.slot_ptr());
            if (!found.isUndefined()) return found.rawBits();
        }
        rtCheckFunctionMember(keyStr);
        return Value::fromUndefined().rawBits();
    }

    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("property access with an unregistered key index");
    // Interned arena key: no allocation on the property path.
    //
    // The RECEIVER is rooted because a read can now run user code: a getter
    // is a call, so this load is a collection point like any other helper
    // call (docs/0006 decision 4), and the raw bits this helper was handed
    // are dead the moment one runs.
    Rooted<Value> objRoot{objVal};
    Rooted<Value> key(Value::fromString(keyHeader));
    Value result = objRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), key, ic);
    // A namespace object is an ordinary object, so a member it does not carry
    // reads `undefined` like any other miss — which for a name ECMA-262 says
    // exists is the silent lie rt_members.cpp exists to prevent. Checked only
    // on the miss, so the hit path is untouched.
    if (result.isUndefined()) rtMathCheckMissingMember(objRoot.get(), keyStr);
    return result.rawBits();
}

// `super.k` — a read of the PARENT prototype's property, with `this` as the
// receiver (ECMA-262 13.3.7.3, MakeSuperPropertyReference). For a method the
// receiver makes no difference: the value is the same function object either
// way, which is why lowering could spell `super.m` as an ordinary read for as
// long as bronze had no accessors. For a GETTER it is the whole difference —
// running it against the prototype would read the prototype's fields on every
// instance, silently.
uint64_t bronze_super_get(uint64_t protoBits, uint32_t keyIndex, uint64_t thisBits) {
    Value protoVal(protoBits);
    if (!protoVal.isObject() ||
        protoVal.asObject<HeapObjectHeader>()->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        fatal("internal: super property read on a base whose prototype is not an object");
    }
    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("super property read with an unregistered key index");

    Rooted<Value> receiver{Value(thisBits)};
    Rooted<Value> protoRoot{protoVal};
    Rooted<Value> key(Value::fromString(keyHeader));
    // No inline cache: an entry describes ONE shape, and this read has two
    // objects — the holder it walks from and the receiver it runs against.
    return protoRoot.get()
        .asObject<ObjectHeader>()
        ->getProp(rtHeap(), key, /*ic=*/nullptr, receiver.slot_ptr())
        .rawBits();
}

void bronze_prop_set(uint64_t objBits, uint32_t keyIndex, uint64_t valBits, uint64_t* icEntry) {
    Value objVal(objBits);
    Value valVal(valBits);
    InlineCache* ic = asCache(icEntry);
    // Writing a property of null or undefined is the same TypeError as
    // reading one (ECMA-262 7.3.4), and discarding the write is worse than
    // reading `undefined`: the program believes it stored something.
    if (objVal.isNull() || objVal.isUndefined()) {
        rtThrowTypeError("Cannot set properties of " +
                         std::string(objVal.isNull() ? "null" : "undefined") + " (setting '" +
                         rtKeyString(keyIndex) + "')");
        return;
    }
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
            propsRoot.get().asObject<ObjectHeader>()->setProp(
                rtHeap(), rtArena(), key, val, /*ic=*/nullptr, /*enumerable=*/true,
                /*defineOwn=*/false, fnRoot.slot_ptr());
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

// A class method, installed on a prototype (or, for a `static`, on the
// constructor's own-property object). Not `bronze_prop_set`: ECMA-262 15.7.14
// defines a method with `enumerable: false`, and an ordinary assignment
// creates an enumerable property. Its own helper rather than a flag on the
// setter because it has no inline cache — a class body runs once, so the site
// is cold by construction, and giving it a cache entry would spend a slot in
// the module's IC table on a write that never repeats.
void bronze_method_def(uint64_t objBits, uint32_t keyIndex, uint64_t valBits) {
    Value objVal(objBits);
    if (!objVal.isObject()) {
        fatal("internal: a class method defined on a value that is not an object");
    }
    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("class method definition with an unregistered key index");

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    Rooted<Value> val{Value(valBits)};
    if (hdr->flags == 2) {  // a `static` member: an own property of the function
        Rooted<Value> fnRoot{objVal};
        rtEnsureFunctionProperties(fnRoot);
        Rooted<Value> propsRoot{fnRoot.get().asObject<FunctionHeader>()->properties};
        Rooted<Value> key(Value::fromString(rtKeyHeader(keyIndex)));
        propsRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val,
                                                          /*ic=*/nullptr,
                                                          /*enumerable=*/false,
                                                          /*defineOwn=*/true);
        return;
    }
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        fatal("internal: a class method defined on a receiver that is not a plain object");
    }
    Rooted<Value> objRoot{objVal};
    Rooted<Value> key(Value::fromString(keyHeader));
    // A DEFINITION, not an assignment: a base class's `set m(v)` must not
    // swallow a derived class's `m() {}` (ECMA-262 15.7.14 defines a method
    // with DefineMethod, which never consults the prototype chain).
    objRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val,
                                                    /*ic=*/nullptr, /*enumerable=*/false,
                                                    /*defineOwn=*/true);
}

// `get k() {}` / `set k(v) {}`, in an object literal or a class body. One
// helper for both halves and both places, because they define one property
// either way; `enumerable` is the whole difference between the two places
// (ECMA-262 13.2.5.5 says an object literal's accessor is enumerable, 15.7.14
// says a class's is not — the same split methods already have).
//
// No inline cache, for the reason bronze_method_def has none: a literal or a
// class body defines each accessor once, so there is no repeat to cache.
void bronze_accessor_def(uint64_t objBits, uint32_t keyIndex, uint64_t getterBits,
                         uint64_t setterBits, bool enumerable) {
    Value objVal(objBits);
    if (!objVal.isObject()) {
        fatal("internal: an accessor defined on a value that is not an object");
    }
    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("accessor definition with an unregistered key index");

    Rooted<Value> getter{Value(getterBits)};
    Rooted<Value> setter{Value(setterBits)};
    Rooted<Value> key(Value::fromString(keyHeader));

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags == 2) {  // `static get k()`: an own property of the function
        Rooted<Value> fnRoot{objVal};
        rtEnsureFunctionProperties(fnRoot);
        Rooted<Value> propsRoot{fnRoot.get().asObject<FunctionHeader>()->properties};
        ObjectHeader::defineAccessor(rtHeap(), rtArena(), propsRoot, key, getter, setter,
                                     enumerable);
        return;
    }
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        fatal("an accessor property on an array or a typed array is unsupported");
    }
    Rooted<Value> objRoot{objVal};
    ObjectHeader::defineAccessor(rtHeap(), rtArena(), objRoot, key, getter, setter, enumerable);
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
    if (hdr->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        // A plain object stores everything by NAME, so a computed key is a
        // property read with a key the compiler did not know: ToPropertyKey,
        // then the ordinary lookup. No inline cache, because there is no
        // per-site key to cache against.
        Rooted<Value> objRoot{objVal};
        Rooted<Value> key{elemKeyAsString(Value(idxBits))};
        return objRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), key).rawBits();
    }
    fatal("computed index access is only supported on arrays, plain objects "
          "and Float32Array");
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
    if (hdr->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        Rooted<Value> objRoot{objVal};
        Rooted<Value> val{Value(valBits)};
        Rooted<Value> key{elemKeyAsString(Value(idxBits))};
        objRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
        return;
    }
    fatal("computed index writes are only supported on arrays, plain objects "
          "and Float32Array");
}

}  // extern "C"

}  // namespace bronze::runtime
