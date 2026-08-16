// Property and element READS: the `o.k` and `o[i]` halves of the ABI, and the
// key decoding both directions share.
//
// Each receiver kind is its own branch because each stores properties
// differently — an array in its elements, a typed array in its buffer, a
// function in its prototype slot and own-property object, a plain object in
// its shape and slots. A name the receiver's prototype really defines and
// bronze has not built is diagnosed by rt_members.cpp rather than read as
// `undefined`.
//
// A PRIMITIVE receiver is the one kind that is not here, and the seam is that
// same sentence read backwards: it stores nothing, so its answer comes from an
// intrinsic prototype rather than from the value. rt_prop_primitive.cpp owns it.
//
// The WRITE dispatch is rt_prop_write.cpp, split off along the same kind of
// seam: a read asks every receiver the same question and takes each kind's
// answer, while a write asks whether the receiver can hold the property at all
// — so that file is almost entirely refusals and this one is almost entirely
// lookups. What they SHARE is the key — whether it names an element and what
// string a computed one names mean the same thing in either direction — and
// that is rt_key.cpp, reached by both through rt_internal.h so that neither can
// keep a second opinion about `a["01"]`.

#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

#include "abi/bronze_abi.h"
#include "runtime/accessor.h"
#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/profile.h"
#include "runtime/ic_log.h"
#include "runtime/exception.h"
#include "runtime/proxy.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/integrity.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/number_format.h"
#include "runtime/object.h"
#include "runtime/namespace.h"
#include "runtime/promise.h"
#include "runtime/regexp.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

// The IC table is a zero-initialized global array in the GENERATED object file,
// one entry per property site, and `rtAsCache` (rt_internal.h) takes the entry
// pointer. That is what lets compiled code hold a stable address per site and
// inline the shape check, which a std::vector — which reallocates — could never
// offer.
//
// `entry` is null only when a caller has no site to cache against (the
// runtime's own property paths); ObjectHeader::getProp already treats a null
// cache as "look it up and cache nothing", a difference in speed and not in
// semantics.

// A member of a WeakMap or a WeakSet, by name. The Map arrangement below with
// the `size` line missing — 24.3.3 and 24.4.3 define no such accessor, so its
// absence is the language's answer and not a gap.
static Value weakCollectionMemberByName(Rooted<Value>& recv, const std::string& keyStr) {
    const bool weakSet =
        recv.get().asObject<HeapObjectHeader>()->flags == MapHeader::kWeakSetFlags;
    Value method = rtWeakCollectionMethod(weakSet, keyStr);
    if (!method.isUndefined()) return method;
    rtCheckWeakCollectionMember(weakSet, keyStr);
    return rtObjectProtoMember(recv, keyStr);
}

// A member of a Map or a Set, by name. Its own function because BOTH `m.get`
// and `m[k]` reach it: a Map's keys are values and its members are names, so
// the computed-index path cannot treat the key as an element the way it does
// for an array.
static Value mapMemberByName(Rooted<Value>& recv, const std::string& keyStr) {
    const bool set = recv.get().asObject<HeapObjectHeader>()->flags == MapHeader::kSetFlags;
    // `size` is an ACCESSOR in the specification (24.1.3.10) and a plain read
    // here: bronze has no Map.prototype for a getter to live on, and the
    // observable difference — `Object.getOwnPropertyDescriptor` of it — is
    // unreachable, since a Map has no own properties at all.
    if (keyStr == "size") {
        return Value::fromDouble(recv.get().asObject<MapHeader>()->liveSize());
    }
    Value method = rtMapMethod(set, keyStr);
    if (!method.isUndefined()) return method;
    rtCheckMapMember(set, keyStr);
    // 24.1.3 / 24.2.3 name nothing else, and the chain does not stop there:
    // `m.hasOwnProperty` and `m.toString` are `Object.prototype`'s, one link up.
    return rtObjectProtoMember(recv, keyStr);
}

// What a WELL-KNOWN symbol names on a receiver that carries no shape.
//
// `o[sym]` on a plain object or a function is a slot the shape decides, and
// needs nothing from here. An array, a string, a typed array, a Map and a Set
// have no own properties at all, so what `v[Symbol.iterator]` means for them is
// ECMA-262's answer rather than the object's — and it used to arrive through
// the string member tables purely because the key used to be the string
// `"@@iterator"`. This is the one place that sees both the receiver kind and
// the key, so it is where that answer moved to.
//
// `handled` separates "this receiver kind has no answer here" from "the answer
// is undefined", which are the same bits and different facts: only the first
// may fall through to the shape.
// 20.4.2.14 `@@toStringTag` for a receiver that has NO PROTOTYPE OBJECT to
// carry it.
//
// This is an approximation of the MECHANISM and not of the bytes. ECMA-262
// puts the tag on a prototype — `Map.prototype[@@toStringTag]` is "Map"
// (24.1.3.13), `Set.prototype`'s is "Set" (24.2.3.12),
// `%TypedArray%.prototype`'s is an accessor over [[TypedArrayName]]
// (23.2.3.35), `ArrayBuffer.prototype`'s is "ArrayBuffer" (25.1.6.6),
// `DataView.prototype`'s is "DataView" (25.3.4.25) — and bronze has none of
// those objects, so the property a walk would find is answered from the heap
// kind at the one place that sees both the receiver and the key. The VALUE is
// the one the specification's property holds, so
// `Object.prototype.toString.call(new Map())` is the spec's bytes.
//
// Nothing a program installs is overridden by this, because it answers only for
// receivers that have no shape to install anything on: a plain object's and a
// function's own `[Symbol.toStringTag]` is a slot, found by the ordinary walk
// that this function declines (`handled` stays false) to intercept.
//
// One of the answers is not an approximation at all: a module namespace's is an
// OWN property that 10.4.6.1 defines on the object itself.
//
// A PRIMITIVE is not this function's business any more. All four kinds reach a
// real intrinsic prototype now, and the walk below finds what that object
// carries — "Symbol" from `Symbol.prototype[@@toStringTag]` (20.4.3.6), and
// nothing from the other three, because 21.1.3, 22.1.3 and 20.3.3 define no tag
// and the language's answer really is `undefined`.
Value toStringTagOf(Value objVal, bool& handled) {
    if (!objVal.isObject()) return Value::fromUndefined();
    switch (objVal.asObject<HeapObjectHeader>()->flags) {
        case MapHeader::kMapFlags:
            handled = true;
            return rtMakeString("Map");
        case MapHeader::kSetFlags:
            handled = true;
            return rtMakeString("Set");
        case MapHeader::kWeakMapFlags:
            handled = true;
            return rtMakeString("WeakMap");  // 24.3.3.6
        case MapHeader::kWeakSetFlags:
            handled = true;
            return rtMakeString("WeakSet");  // 24.4.3.5
        case TypedArrayHeader::kFlags: {
            // 23.2.3.35 is an ACCESSOR whose answer is [[TypedArrayName]], so
            // nine views give nine tags rather than one shared "TypedArray".
            const char* kind =
                reinterpret_cast<TypedArrayHeader*>(objVal.asObject<HeapObjectHeader>())
                    ->kindName();
            handled = true;
            return rtMakeString(kind);
        }
        case ArrayBufferHeader::kFlags:
            handled = true;
            return rtMakeString("ArrayBuffer");
        case DataViewHeader::kFlags:
            handled = true;
            return rtMakeString("DataView");
        case ModuleNamespaceHeader::kFlags:
            handled = true;
            return rtMakeString("Module");
        case HeapKind::Function:
            if (objVal.asObject<FunctionHeader>()->is_generator) {
                handled = true;
                return rtMakeString("GeneratorFunction");
            }
            return Value::fromUndefined();
        default:
            // An array, a function, a RegExp and a plain object: 23.1.3, 20.2.3,
            // 22.2.6 and 20.1.3 define no `@@toStringTag` at all, which is
            // exactly why 20.1.3.6 keeps a builtin-tag list for them.
            return Value::fromUndefined();
    }
}

Value wellKnownSymbolMember(Value objVal, Value keyVal, bool& handled) {
    handled = false;
    if (keyVal.asSymbol<SymbolHeader>() == rtSymbolToStringTag()) {
        return toStringTagOf(objVal, handled);
    }
    if (keyVal.asSymbol<SymbolHeader>() == rtSymbolSpecies()) {
        if (rtIsArrayConstructor(objVal) ||
            rtIsArrayBufferConstructor(objVal) ||
            rtIsTypedArrayConstructor(objVal) ||
            rtIsRegExpConstructor(objVal)) {
            handled = true;
            return objVal;
        }
        return Value::fromUndefined();
    }
    if (keyVal.asSymbol<SymbolHeader>() == rtSymbolHasInstance()) {
        if (objVal.isObject() && objVal.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
            handled = true;
            return rtNativeFunction(rtFunctionHasInstanceBuiltin, 1);
        }
        return Value::fromUndefined();
    }
    if (keyVal.asSymbol<SymbolHeader>() != rtSymbolIterator()) return Value::fromUndefined();
    // A STRING is not this function's business: 22.1.3.36 puts its
    // `[Symbol.iterator]` on the real `String.prototype` object
    // (builtin_string_iterator.cpp), and the ordinary symbol-keyed walk below
    // this dispatch finds it there — `handled` stays false, exactly as it does
    // for every other member a string reaches through its intrinsic.
    if (!objVal.isObject()) return Value::fromUndefined();
    switch (objVal.asObject<HeapObjectHeader>()->flags) {
        case HeapKind::Array:
            // 23.1.3.41 makes it the same function object as
            // `Array.prototype.values` — an IDENTITY, not a twin, and it holds
            // because both routes intern on the one code pointer.
            handled = true;
            return rtNativeFunction(rtArrayValuesBuiltin, 0);
        case TypedArrayHeader::kFlags:
            handled = true;
            return rtTypedArrayIteratorMethod();
        case MapHeader::kMapFlags:
            handled = true;
            return rtMapDefaultIterator(/*isSetReceiver=*/false);
        case MapHeader::kSetFlags:
            handled = true;
            return rtMapDefaultIterator(/*isSetReceiver=*/true);
        default:
            return Value::fromUndefined();
    }
}

// The plain object a receiver keeps SYMBOL-keyed properties on: itself, or —
// for a function / array — the side object its properties live in.
ObjectHeader* rtSymbolKeyHolder(Value objVal) {
    if (!objVal.isObject()) return nullptr;
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) return reinterpret_cast<ObjectHeader*>(hdr);
    if (hdr->flags == HeapKind::Function) {
        Value props = objVal.asObject<FunctionHeader>()->properties;
        return props.isObject() ? props.asObject<ObjectHeader>() : nullptr;
    }
    if (hdr->flags == HeapKind::Array) {
        Value props = objVal.asObject<ArrayHeader>()->properties;
        return props.isObject() ? props.asObject<ObjectHeader>() : nullptr;
    }
    return nullptr;
}

namespace {

// Where a symbol-keyed READ starts its prototype walk. It is deliberately NOT
// the function above, and the difference is the point: a primitive has no
// storage of its own to write a symbol-keyed property INTO, but it does have a
// chain to read one OFF — which is how `sym[Symbol.toStringTag]` reaches
// 20.4.3.6's "Symbol" on `Symbol.prototype`. One function serving both would
// make `sym[k] = v` write onto the intrinsic every symbol in the program
// shares.
//
// ALLOCATES, because the first read of any intrinsic builds it — so the caller
// roots its receiver and its key before asking, and the raw header the tail
// derives is read after everything that can move it.
Value symbolReadStart(Value v) {
    if (v.isString()) return rtStringPrototype();
    if (v.isBool()) return rtBooleanPrototype();
    if (v.isNumber()) return rtNumberPrototype();
    if (v.isSymbol()) return rtSymbolPrototype();
    // The same road as the four above it, and the reason a BigInt needs it:
    // `Object.prototype.toString.call(1n)` reads @@toStringTag off the
    // receiver, and 21.2.3.5 puts "BigInt" on `BigInt.prototype`.
    if (v.isBigInt()) return rtBigIntPrototype();
    ObjectHeader* holder = rtSymbolKeyHolder(v);
    return holder ? Value::fromObject(holder) : Value::fromUndefined();
}

}  // namespace

extern "C" {

// A property read by NAME, with the receiver-kind dispatch that `o.k` and
// `o[k]` must share. They reach it from two different places - one with the
// key the compiler registered, one with the key ToPropertyKey just produced -
// and a second copy of this dispatch would be a second answer to "does this
// member exist?", which is the question rt_members.cpp exists to keep one of.
static uint64_t propGetByName(Value objVal, const std::string& keyStr, StringHeader* keyHeader,
                              InlineCache* ic);

uint64_t bronze_prop_get(uint64_t objBits, uint32_t keyIndex, uint64_t* icEntry) {
    recordPropCall("bronze_prop_get", keyIndex, icEntry);
    recordPropGetMiss(objBits, keyIndex, icEntry);
    Value objVal(objBits);
    InlineCache* ic = rtAsCache(icEntry);

    // IC-hit fast path first: a shape match needs no key at all. Generated code
    // inlines the depth-0/inline-slot corner of exactly this check and only
    // calls in when that misses, so what remains hot here is the proto-hit and
    // overflow-slot case.
    if (objVal.isObject()) {
        HeapObjectHeader* fastHdr = objVal.asObject<HeapObjectHeader>();
        if (fastHdr->flags == HeapKind::Plain) {
            if (ic && ic->cached_shape) {
                auto* fastObj = reinterpret_cast<ObjectHeader*>(fastHdr);
                if (ic->describes(fastObj->shape)) {
                    if (ic->isAccessor()) {
                        uint32_t depth = ic->realDepth();
                        ObjectHeader* holder = fastObj;
                        if (depth > 0) {
                            bool crossedDictionary = false;
                            holder = fastObj->cachedProtoHolder(depth, crossedDictionary);
                        }
                        if (holder) {
                            Value getter = holder->getSlot(ic->cached_slot);
                            if (getter.isObject() &&
                                getter.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
                                FunctionHeader* fn = getter.asObject<FunctionHeader>();
                                if (fn->code && fn->arity == 0) {
                                    return fn->code(fn->env_record.rawBits(), objBits, 0, nullptr);
                                }
                            }
                            Rooted<Value> self{objVal};
                            return callGetter(getter, self).rawBits();
                        }
                    } else {
                        if (ic->cached_depth == 0) return fastObj->getSlot(ic->cached_slot).rawBits();
                        bool crossedDictionary = false;
                        if (ObjectHeader* holder =
                                fastObj->cachedProtoHolder(ic->cached_depth, crossedDictionary)) {
                            return holder->getSlot(ic->cached_slot).rawBits();
                        }
                    }
                }
            }
        } else if (fastHdr->flags == HeapKind::Array) {
            if (ic && ic->isArrayMethod() && bronze_array_method_ic_enabled != 0) {
                const auto* arr = reinterpret_cast<const ArrayHeader*>(fastHdr);
                if (!arr->properties.isObject()) {
                    return bronze_array_method_tbl[ic->cached_slot];
                }
            }
            const KeyInfo& ki = rtKeyInfo(keyIndex);
            if (ki.isElemIndex) {
                return reinterpret_cast<const ArrayHeader*>(fastHdr)->getElem(ki.elemIndex).rawBits();
            }
            if (ki.isLength) {
                return Value::fromDouble(reinterpret_cast<const ArrayHeader*>(fastHdr)->length).rawBits();
            }
        } else if (fastHdr->flags == TypedArrayHeader::kFlags) {
            const KeyInfo& ki = rtKeyInfo(keyIndex);
            if (ki.isElemIndex) {
                const auto* view = reinterpret_cast<const TypedArrayHeader*>(fastHdr);
                if (ki.elemIndex >= view->length) return Value::fromUndefined().rawBits();
                return Value::fromDouble(view->get(ki.elemIndex)).rawBits();
            }
            if (ki.isLength) {
                return Value::fromDouble(reinterpret_cast<const TypedArrayHeader*>(fastHdr)->length).rawBits();
            }
            StringHeader* keyHeader = rtKeyHeader(keyIndex);
            if (keyHeader && keyHeader->isLatin1()) {
                const size_t kLen = keyHeader->getLength();
                const char* kData = keyHeader->latin1Data();
                const auto* view = reinterpret_cast<const TypedArrayHeader*>(fastHdr);
                if (kLen == 10 && std::memcmp(kData, "byteLength", 10) == 0) {
                    return Value::fromDouble(view->byteLength()).rawBits();
                }
                if (kLen == 10 && std::memcmp(kData, "byteOffset", 10) == 0) {
                    return Value::fromDouble(view->byteOffset).rawBits();
                }
                if (kLen == 6 && std::memcmp(kData, "buffer", 6) == 0) {
                    return view->buffer.rawBits();
                }
            }
        }
    }

    return propGetByName(objVal, rtKeyString(keyIndex), rtKeyHeader(keyIndex), ic);
}

static uint64_t propGetByName(Value objVal, const std::string& keyStr, StringHeader* keyHeader,
                              InlineCache* ic) {
    // The interned key is needed by more than the plain-object branch now, so
    // its registration is checked once at the top rather than where it is
    // first read.
    if (!keyHeader) fatal("property access with an unregistered key index");

    // Reading a property of null or undefined is a TypeError in ECMA-262 7.3.2
    // (GetV -> ToObject), and answering `undefined` for it is the
    // silent-wrong-answer shape CLAUDE.md forbids: `a.b.c` where `a.b` is
    // missing would report nothing and carry an undefined onward. It is also
    // what makes `(a?.b).c` differ observably from `a?.b.c`. Catchable since
    // The spec names it, so it is a thrown TypeError rather than the process
    // death it used to be.
    if (objVal.isNull() || objVal.isUndefined()) {
        return rtThrowTypeError("Cannot read properties of " +
                                std::string(objVal.isNull() ? "null" : "undefined") +
                                " (reading '" + keyStr + "')")
            .rawBits();
    }
    // Every other PRIMITIVE receiver, whose answer comes from an intrinsic
    // rather than from the value — rt_prop_primitive.cpp owns that whole
    // question, including the index properties 10.4.3.5 synthesises for a
    // string.
    if (!objVal.isObject()) {
        return rtPrimitiveMember(objVal, keyStr, keyHeader, ic).rawBits();
    }

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    uint32_t idx = 0;

    if (hdr->flags == HeapKind::Array) {
        // Rooted for the tail: everything from the property-object walk onward
        // can allocate, and the `Object.prototype` step below needs the
        // receiver AFTER those allocations have possibly moved it. `arr` is
        // read only above the first of them.
        Rooted<Value> recv{objVal};
        ArrayHeader* arr = recv.get().asObject<ArrayHeader>();
        if (keyStr == "length") return Value::fromDouble(arr->length).rawBits();
        if (rtKeyAsIndex(keyStr, idx)) return arr->getElem(idx).rawBits();
        // A named own property: a match array's `index`, an `arguments`
        // object's `callee`, or anything a program assigned. Read BEFORE the
        // prototype methods, because an own property SHADOWS an inherited one —
        // `a.map = 5` reads 5, and `m.index` must not answer with
        // `Array.prototype.index` if one is ever added.
        //
        // The presence test is the shape's and not "the value is not
        // undefined": `a.map = undefined` is an own property whose value is
        // undefined, and reading the builtin for it would un-shadow a property
        // the program really created.
        if (PropertyInfo info; rtArrayOwnNamed(recv.get(), keyHeader, info)) {
            Rooted<Value> propsRoot{recv.get().asObject<ArrayHeader>()->properties};
            Rooted<Value> key(Value::fromString(keyHeader));
            // The array is the receiver, so a getter stored here runs against
            // the object the program read from rather than against the box.
            return propsRoot.get()
                .asObject<ObjectHeader>()
                ->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr())
                .rawBits();
        }
        if (uint32_t methodId = rtArrayMethodId(keyStr); methodId != UINT32_MAX) {
            Value method = rtArrayMethodById(methodId);
            if (!method.isUndefined()) {
                if (ic && bronze_array_method_ic_enabled != 0 &&
                    !recv.get().asObject<ArrayHeader>()->properties.isObject()) {
                    ic->fillArrayMethod(methodId);
                }
                return method.rawBits();
            }
        }
        rtCheckArrayMember(keyStr);
        // `Array.prototype`'s own members have all had their say — including
        // the two that SHADOW this next step, `toString` and `toLocaleString`,
        // which the table above refuses by name. So what is left is the chain
        // above it.
        return rtObjectProtoMember(recv, keyStr).rawBits();
    }
    if (hdr->flags == TypedArrayHeader::kFlags) {
        // The index is tried FIRST: `v[0]` is the whole point of a typed array
        // and must not walk a member table on the way to the element.
        auto* view = reinterpret_cast<TypedArrayHeader*>(hdr);
        if (rtKeyAsIndex(keyStr, idx)) {
            // Out of range is `undefined` and not an error — a typed array has
            // no elements outside its length and nowhere to continue the search
            // (10.4.5.4 makes a canonical numeric string absent rather than
            // inherited, which is why this returns instead of falling through
            // to the chain below).
            if (idx >= view->length) return Value::fromUndefined().rawBits();
            return Value::fromDouble(view->get(idx)).rawBits();
        }
        Rooted<Value> recv{objVal};
        const Value found = rtTypedArrayMember(recv.get(), keyStr);
        if (!found.isUndefined()) return found.rawBits();
        return rtObjectProtoMember(recv, keyStr).rawBits();
    }
    if (hdr->flags == ArrayBufferHeader::kFlags) {
        Rooted<Value> recv{objVal};
        const Value found = rtArrayBufferMember(recv.get(), keyStr);
        if (!found.isUndefined()) return found.rawBits();
        return rtObjectProtoMember(recv, keyStr).rawBits();
    }
    if (hdr->flags == DataViewHeader::kFlags) {
        // No index branch above this one, unlike a typed array's: 25.3 defines
        // no integer-indexed access at all, so `view[0]` is an ordinary
        // property name that DataView does not define and the chain answers.
        Rooted<Value> recv{objVal};
        const Value found = rtDataViewMember(recv.get(), keyStr);
        if (!found.isUndefined()) return found.rawBits();
        return rtObjectProtoMember(recv, keyStr).rawBits();
    }
    if (hdr->flags == MapHeader::kMapFlags || hdr->flags == MapHeader::kSetFlags) {
        Rooted<Value> recv{objVal};
        return mapMemberByName(recv, keyStr).rawBits();
    }
    if (hdr->flags == MapHeader::kWeakMapFlags || hdr->flags == MapHeader::kWeakSetFlags) {
        Rooted<Value> recv{objVal};
        return weakCollectionMemberByName(recv, keyStr).rawBits();
    }
    if (hdr->flags == RegExpHeader::kFlags) {
        // Every member of a RegExp is computed from the header and the
        // compiled pattern; there is no shape and no slot to read, which is
        // why this is a branch here rather than properties on an object.
        Rooted<Value> recv{objVal};
        const Value found = rtRegExpMember(recv.get(), keyStr);
        if (!found.isUndefined()) return found.rawBits();
        return rtObjectProtoMember(recv, keyStr).rawBits();
    }
    if (hdr->flags == IterRecordHeader::kFlags) {
        // The record of a live for-of is not a JS value: nothing hands one to
        // a program, so reaching this is a lowering bug rather than something
        // a program did.
        fatal("internal: a property read on an iteration record");
    }
    if (hdr->flags == ModuleNamespaceHeader::kFlags) {
        // 10.4.6.7. A namespace has no prototype (10.4.6.1 fixes [[Prototype]]
        // at null), so there is no chain to continue on and a name it does not
        // export is `undefined` here rather than a step further up.
        Value found = Value::fromUndefined();
        // Cannot answer false — the flags word above is what it tests — so this
        // returns unconditionally rather than falling through to a cast that
        // would read a namespace's payload as an ObjectHeader's shape word.
        rtModuleNamespaceGet(objVal, keyHeader, found);
        return found.rawBits();
    }
    if (hdr->flags == HeapKind::Proxy) {
        // 10.5.8: the `get` trap, or the target's read through this same
        // funnel. The proxy has no shape of its own, so nothing below this
        // dispatch could answer for it anyway.
        return rtProxyGet(objVal, Value::fromString(keyHeader)).rawBits();
    }
    if (hdr->flags == HeapKind::Function) {
        // Rooted for the same reason the array branch is: the tail below walks
        // `Object.prototype`, and everything between here and there allocates.
        Rooted<Value> recv{objVal};
        // A GLOBAL CONSTRUCTOR's statics come first, ahead of the `prototype`
        // slot below. That order is the whole point: a FunctionHeader answers
        // `prototype` from a slot it creates on demand, so `Array.prototype`
        // would read as an empty object — a silent lie about an intrinsic
        // bronze does not have, and one a program could install a method on
        // that nothing would ever find.
        if (Value ctorMember; rtGlobalConstructorMember(recv.get(), keyStr, ctorMember)) {
            return ctorMember.rawBits();
        }
        // `prototype` lives in its own slot; every other own property lives in
        // the function's property object and is found through ITS prototype
        // chain, which `extends` linked to the base class's. Reading
        // `prototype` first is what keeps `call`, `bind` and `name` answered
        // or diagnosed rather than read as undefined.
        if (keyStr == "prototype") {
            if (rtIsFunctionPrototype(recv.get())) {
                return Value::fromUndefined().rawBits();
            }
            if (rtIsFunctionConstructor(recv.get())) {
                return rtFunctionPrototypeObject().rawBits();
            }
            // The guard above only covers `kCtors`. Map, Set, ArrayBuffer and
            // the nine views are interned function singletons of their own, so
            // without this they reached the on-demand slot below and
            // `Map.prototype` answered a fresh empty object — the exact lie
            // the comment above says the ordering exists to prevent, told
            // about every intrinsic that is not one of the three. Named here
            // rather than by adding `prototype` to nine more tables, because
            // the property is absent for the same one reason each time.
            const char* intrinsic = rtMapConstructorName(recv.get());
            if (!intrinsic) intrinsic = rtWeakCollectionConstructorName(recv.get());
            if (!intrinsic) intrinsic = rtTypedArrayConstructorName(recv.get());
            if (!intrinsic) intrinsic = rtDataViewConstructorName(recv.get());
            if (intrinsic) {
                fatal((std::string("unsupported: ") + intrinsic +
                       ".prototype is not implemented (bronze has no prototype OBJECT for this "
                       "intrinsic; its methods are answered by the property path)")
                          .c_str());
            }
            rtEnsureFunctionPrototype(recv);
            return recv.get().asObject<FunctionHeader>()->prototype.rawBits();
        }
        Value props = recv.get().asObject<FunctionHeader>()->properties;
        if (props.isObject()) {
            // The receiver a `static get` sees is the CLASS, not the side
            // object its statics are kept in — which is the whole reason
            // getProp takes a receiver at all.
            Rooted<Value> propsRoot{props};
            ObjectHeader* propsObj = propsRoot.get().asObject<ObjectHeader>();
            PropertyInfo own;
            if (propsObj->shape &&
                propsObj->shape->lookupProperty(
                    PropertyKey::forString(keyHeader), own)) {
                Rooted<Value> key(Value::fromString(keyHeader));
                return propsObj->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr()).rawBits();
            }
        }
        // `length` and `name`, the two own data properties 10.2.10 and 10.2.9
        // give every function object. Both are non-writable and non-enumerable,
        // and both live in the header rather than in the statics table above:
        // they are created by OrdinaryFunctionCreate before any `static` can be
        // written, and a program cannot overwrite either (rt_prop_write.cpp
        // refuses the assignment).
        //
        // They are read AFTER the statics all the same, and that order is the
        // language's: `class C { static name() {} }` DEFINES a `name` property
        // over the one 15.7.14 step 15 had just given the constructor, so the
        // method wins. An assignment could not have put anything there, so the
        // only thing this order can find first is a definition that really did
        // replace the property.
        if (const FunctionHeader* fn = recv.get().asObject<FunctionHeader>(); fn->name) {
            if (keyStr == "length") return Value::fromDouble(fn->length).rawBits();
            if (keyStr == "name") return rtCopyKeyToHeap(fn->name).rawBits();
        } else if (keyStr == "length" || keyStr == "name") {
            // A function bronze did not compile: a native builtin, or a method
            // whose key is computed at run time. rt_members.cpp's table would
            // report the member "not implemented", which is the wrong sentence
            // now that it is — what is missing is this function's own answer.
            fatal((std::string("unsupported: `") + keyStr +
                   "` of a function whose name bronze never recorded (a built-in, or a member "
                   "whose key is computed at run time; a function the compiler created answers "
                   "both)")
                      .c_str());
        }
        // `Symbol` is a function object so that `Symbol("tag")` names bronze
        // rather than reporting that an object is not callable, which means its
        // unimplemented members reach the FUNCTION miss path rather than a
        // namespace object's. 23.2.6.2's own data property, before the
        // Function.prototype table: a typed-array constructor really carries
        // it, so answering `undefined` would be a silent lie about a name
        // ECMA-262 defines.
        if (Value stat; rtTypedArrayStatic(recv.get(), keyStr, stat)) return stat.rawBits();
        rtSymbolCheckMissingMember(recv.get(), keyStr);
        // The same step for `Promise`, whose statics live in the properties
        // object read above — so a name 27.2.4 defines and bronze has not
        // built (`withResolvers`, `try`) reaches here having missed, and is
        // refused BY NAME rather than falling through to `undefined` the way
        // every other unknown member of a function object does.
        if (rtIsPromiseConstructor(recv.get())) rtCheckPromiseStaticMember(keyStr);
        // After the own properties above, because a static named `call` shadows
        // the inherited one — which is the ordinary rule, and the reason this
        // is not read first even though it is the cheaper lookup.
        if (Value method = rtFunctionMethod(keyStr); !method.isUndefined()) {
            return method.rawBits();
        }
        if (props.isObject()) {
            Rooted<Value> propsRoot{props};
            ObjectHeader* propsObj = propsRoot.get().asObject<ObjectHeader>();
            const uint64_t objProtoBits = rtObjectPrototype().rawBits();
            for (uint32_t depth = 1; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
                ObjectHeader* ancestor = propsObj->protoAncestor(depth);
                if (!ancestor || Value::fromObject(ancestor).rawBits() == objProtoBits) break;
                PropertyInfo inherited;
                if (ancestor->shape &&
                    ancestor->shape->lookupProperty(PropertyKey::forString(keyHeader), inherited)) {
                    Rooted<Value> key(Value::fromString(keyHeader));
                    return propsObj->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr()).rawBits();
                }
            }
        }
        rtCheckFunctionMember(keyStr);
        // `Function.prototype` has had its say — `call`, `apply` and `bind`
        // answered above, `constructor` and `toString` refused by name just now
        // — so what is left is the object above it. That step is what makes
        // `f.hasOwnProperty` a function rather than `undefined`, which is the
        // one place bronze answered `undefined` for a member of a prototype it
        // HAS: the nearer, unbuilt one was already diagnosed by name.
        return rtObjectProtoMember(recv, keyStr).rawBits();
    }

    // Interned arena key: no allocation on the property path.
    //
    // The RECEIVER is rooted because a read can now run user code: a getter is
    // a call, so this load is a collection point like any other helper call,
    // and the raw bits this helper was handed are dead the moment one runs.
    // Every kind with a branch above returned; anything else reaching the
    // plain-object tail would be read through an ObjectHeader it is not, so the
    // cast is guarded rather than trusted. `bronze_elem_get` used to hold this
    // guarantee for the computed path and lost it when that path was folded
    // into this one.
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        fatal("internal: a property read on an unknown object kind");
    }
    Rooted<Value> objRoot{objVal};
    // 10.4.3.5 StringGetOwnProperty, ahead of the ordinary lookup for a String
    // exotic object. The order is 10.4.3's and not an optimisation: the index
    // properties and the `length` 10.4.3.4 synthesises are non-writable and
    // non-configurable, so no own property a program can define shadows them.
    // A key this does not claim falls through to the ordinary walk, which is
    // the half `new String("ab").indexOf` needs and the half bronze had no
    // holder for before `String.prototype` became an object.
    if (Value exotic; rtStringExoticOwnProperty(objRoot.get(), keyStr, exotic)) {
        return exotic.rawBits();
    }
    Rooted<Value> key(Value::fromString(keyHeader));
    Value result = objRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), key, ic);
    // A namespace object is an ordinary object, so a member it does not carry
    // reads `undefined` like any other miss — which for a name ECMA-262 says
    // exists is the silent lie rt_members.cpp exists to prevent. Checked only
    // on the miss, so the hit path is untouched.
    if (result.isUndefined()) {
        rtMathCheckMissingMember(objRoot.get(), keyStr);
        rtObjectCheckMissingMember(objRoot.get(), keyStr);
        rtJsonCheckMissingMember(objRoot.get(), keyStr);
        // The `Array.prototype` object, whose misses are Array's table: a name
        // 23.1.3 defines and bronze has not built must be as loud read off the
        // object as it is read off an array.
        rtArrayPrototypeCheckMissingMember(objRoot.get(), keyStr);
        // And the chain's own end: a 20.1.3 member of `Object.prototype` that
        // bronze has not built. Applied to every plain object because every
        // plain object inherits from it — an own or nearer property of the same
        // name was found above and never reaches here.
        rtObjectProtoCheckMissingMember(keyStr);
    }
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
    recordPropCall("bronze_super_get", keyIndex, nullptr);
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

void bronze_super_set(uint64_t protoBits, uint32_t keyIndex, uint64_t thisBits,
                      uint64_t valBits) {
    recordPropCall("bronze_super_set", keyIndex, nullptr);
    Value protoVal(protoBits);
    if (!protoVal.isObject() ||
        protoVal.asObject<HeapObjectHeader>()->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        fatal("internal: super property write on a base whose prototype is not an object");
    }
    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("super property write with an unregistered key index");

    Rooted<Value> receiver{Value(thisBits)};
    Rooted<Value> protoRoot{protoVal};
    Rooted<Value> key{Value::fromString(keyHeader)};
    Rooted<Value> val{Value(valBits)};
    protoRoot.get()
        .asObject<ObjectHeader>()
        ->setProp(rtHeap(), rtArena(), key, val, /*ic=*/nullptr, /*enumerable=*/true,
                  /*defineOwn=*/false, receiver.slot_ptr());
}

uint64_t bronze_elem_get(uint64_t objBits, uint64_t idxBits) {
    recordElemCall("bronze_elem_get");
    Value objVal(objBits);

    // 7.1.19 ToPropertyKey, for the one key kind that has to run before
    // anything else: an OBJECT, whose `toString` is user code. It is done here,
    // at the entry, and nowhere below — every branch past this point holds a raw
    // receiver header at some point, and a conversion that collects would move
    // it. The receiver is rooted across the call and the whole helper is
    // re-entered with the converted key, so the fast paths below see exactly the
    // primitive they were written for.
    if (Value(idxBits).isObject()) {
        Rooted<Value> objRoot{objVal};
        Rooted<Value> keyRoot{Value(idxBits)};
        keyRoot.set(rtToPropertyKey(keyRoot));
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        return bronze_elem_get(objRoot.get().rawBits(), keyRoot.get().rawBits());
    }

    // Fast path: numeric index access on an Array or TypedArray (the common case
    // in compute and math kernels). Checked before symbol or string conversion.
    if (objVal.isObject() && idxBits <= kNumberMaxBits) {
        const double d = std::bit_cast<double>(idxBits);
        const uint32_t idx = static_cast<uint32_t>(d);
        if (d >= 0.0 && static_cast<double>(idx) == d && d <= 4294967294.0) {
            HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
            if (hdr->flags == HeapKind::Array) {
                const auto* arr = reinterpret_cast<const ArrayHeader*>(hdr);
                if (idx < arr->length) {
                    const Value v = arr->elementsData()[idx];
                    return v.isHole() ? BRONZE_ABI_UNDEFINED_BITS : v.rawBits();
                }
                return BRONZE_ABI_UNDEFINED_BITS;
            }
            if (hdr->flags == TypedArrayHeader::kFlags) {
                const auto* view = reinterpret_cast<const TypedArrayHeader*>(hdr);
                if (idx >= view->length) return BRONZE_ABI_UNDEFINED_BITS;
                const uint8_t* p = view->bytes() + static_cast<size_t>(idx) * view->bytesPerElement();
                switch (view->elementKind()) {
                    case ElementKind::Float64:
                        return Value::fromDouble(*reinterpret_cast<const double*>(p)).rawBits();
                    case ElementKind::Float32:
                        return Value::fromDouble(static_cast<double>(*reinterpret_cast<const float*>(p))).rawBits();
                    case ElementKind::Int32:
                        return Value::fromDouble(static_cast<double>(*reinterpret_cast<const int32_t*>(p))).rawBits();
                    case ElementKind::Uint32:
                        return Value::fromDouble(static_cast<double>(*reinterpret_cast<const uint32_t*>(p))).rawBits();
                    case ElementKind::Int16:
                        return Value::fromDouble(static_cast<double>(*reinterpret_cast<const int16_t*>(p))).rawBits();
                    case ElementKind::Uint16:
                        return Value::fromDouble(static_cast<double>(*reinterpret_cast<const uint16_t*>(p))).rawBits();
                    case ElementKind::Uint8:
                    case ElementKind::Uint8Clamped:
                        return Value::fromDouble(static_cast<double>(*p)).rawBits();
                    case ElementKind::Int8:
                        return Value::fromDouble(static_cast<double>(*reinterpret_cast<const int8_t*>(p))).rawBits();
                    default:
                        break;
                }
            }
        }
    }

    if (Value(idxBits).isSymbol()) {
        // Reading a property of null or undefined is the TypeError of 7.3.2
        // whatever the key is; the `fatal` below would kill the process where
        // the language throws.
        if (objVal.isNull() || objVal.isUndefined()) {
            return rtThrowTypeError("Cannot read properties of " +
                                    std::string(objVal.isNull() ? "null" : "undefined") +
                                    " (reading a symbol-keyed property)")
                .rawBits();
        }
        // A proxy first: its symbol-keyed answer is the trap's or the
        // target's (10.5.8 makes no distinction by key kind), and neither the
        // well-known-symbol dispatch nor the shape walk below knows how to ask
        // either of them.
        if (objVal.isObject() &&
            objVal.asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) {
            return rtProxyGet(objVal, Value(idxBits)).rawBits();
        }
        if (ObjectHeader* holder = rtSymbolKeyHolder(objVal)) {
            PropertyInfo info;
            if (holder->shape &&
                holder->shape->lookupProperty(
                    PropertyKey::forSymbol(Value(idxBits).asSymbol<SymbolHeader>()), info)) {
                Rooted<Value> recv{objVal};
                Rooted<Value> key{Value(idxBits)};
                Rooted<Value> holderRoot{Value::fromObject(holder)};
                return holderRoot.get()
                    .asObject<ObjectHeader>()
                    ->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr())
                    .rawBits();
            }
        }
        // Rooted because a symbol-keyed property can be an accessor, and a
        // getter is a call — and because the walk below can build an intrinsic,
        // which allocates. No inline cache: a computed site has no entry.
        //
        // The roots are taken BEFORE the well-known dispatch rather than after
        // it, and that order is load-bearing: `wellKnownSymbolMember` allocates
        // on two routes — a well-known symbol is created on first use and its
        // description is a heap string, and `toStringTagOf` builds its answer
        // with `rtMakeString` — so a raw receiver held across the call is a
        // pre-collection address. The first receiver to notice was a Date,
        // whose `@@toPrimitive` lives on its PROTOTYPE, so the lookup falls past
        // the own-shape probe above and through this dispatch before the walk
        // that finds it.
        Rooted<Value> recv{objVal};
        Rooted<Value> key{Value(idxBits)};
        bool handled = false;
        const Value wellKnown = wellKnownSymbolMember(recv.get(), key.get(), handled);
        if (handled) return wellKnown.rawBits();
        Rooted<Value> holderRoot{symbolReadStart(recv.get())};
        // A receiver with neither own symbol-keyed storage nor a chain: an
        // array, a Map, a RegExp. Those have no own symbol-keyed property and
        // no prototype object here to inherit one from.
        if (!holderRoot.get().isObject()) return Value::fromUndefined().rawBits();
        return holderRoot.get()
            .asObject<ObjectHeader>()
            ->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr())
            .rawBits();
    }
    // The two receivers that have ELEMENTS get an index fast path, and only
    // those two: `a[i]` and `v[i]` are the whole reason this helper is not
    // just `bronze_prop_get`, and neither may walk a member table on the way
    // to a slot.
    uint32_t idx = 0;
    if (objVal.isObject()) {
        HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
        Value idxVal(idxBits);
        if (idxVal.isNumber()) {
            double d = idxVal.asNumber();
            if (d >= 0.0 && d <= 4294967294.0) {
                uint32_t u = static_cast<uint32_t>(d);
                if (static_cast<double>(u) == d) {
                    if (hdr->flags == HeapKind::Array) {
                        return reinterpret_cast<ArrayHeader*>(hdr)->getElem(u).rawBits();
                    }
                    if (hdr->flags == TypedArrayHeader::kFlags) {
                        auto* view = reinterpret_cast<TypedArrayHeader*>(hdr);
                        if (u >= view->length) return Value::fromUndefined().rawBits();
                        return Value::fromDouble(view->get(u)).rawBits();
                    }
                }
            }
        }
        if (hdr->flags == HeapKind::Array && rtValueToElementIndex(Value(idxBits), idx)) {
            return reinterpret_cast<ArrayHeader*>(hdr)->getElem(idx).rawBits();
        }
        if (hdr->flags == TypedArrayHeader::kFlags && rtValueToElementIndex(Value(idxBits), idx)) {
            auto* view = reinterpret_cast<TypedArrayHeader*>(hdr);
            // Out of range is `undefined`, not an error — 10.4.5.4 again.
            if (idx >= view->length) return Value::fromUndefined().rawBits();
            return Value::fromDouble(view->get(idx)).rawBits();
        }
    }
    // Everything that is not an index NAMES something, and what a name means
    // cannot depend on whether the compiler knew it: `o.k` and `const s = "k";
    // o[s]` are one question. This branch used to be a second copy of
    // propGetByName's receiver-kind dispatch, and every place the two copies
    // had drifted was a silent wrong answer — `arr[s]` for "push", "length" or
    // "constructor" answered `undefined` where `arr.push` answered the method,
    // and `Math[s]` missed the namespace check that makes `Math.cbrt` a named
    // error. Delegating is what keeps them one question with one answer.
    //
    // A PRIMITIVE receiver reaches it too, which it did not before: `"abc"[i]`
    // died here as "computed index access on a non-object value" while
    // `"abc"[0]` took the name path — one operation with two answers, and the
    // reason `cases/string_index` pins both spellings.
    Rooted<Value> objRoot{objVal};
    Rooted<Value> key{rtElemKeyAsString(Value(idxBits))};
    StringHeader* keyHeader = key.get().asString<StringHeader>();
    return propGetByName(objRoot.get(), rtUtf8Chars(keyHeader), keyHeader, /*ic=*/nullptr);
}

}  // extern "C"

}  // namespace bronze::runtime
