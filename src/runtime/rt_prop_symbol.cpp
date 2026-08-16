// What a receiver with NO PROTOTYPE OBJECT answers for a WELL-KNOWN SYMBOL,
// and where a symbol-keyed read starts its walk.
//
// The seam is the key, not the receiver. rt_prop.cpp dispatches a STRING key on
// the receiver's kind, because each kind stores named properties somewhere
// different. A symbol key stores the same way everywhere — one plain object,
// found by `rtSymbolKeyHolder` — so the interesting question moves to the other
// end: `@@toStringTag`, `@@species`, `@@iterator` and `@@hasInstance` are
// properties of intrinsic PROTOTYPE OBJECTS that bronze does not build, so for
// an array, a Map, a Set, a typed array or a collection there is no object for
// the walk to find them on, and this file stands in for those objects.
//
// Nothing here overrides what a program installs. Every answer is guarded by
// "the receiver has no shape to have installed it on", or — for `@@species`,
// where the receiver is a CONSTRUCTOR and does have one — by an explicit walk
// for a user-defined key first. That is the whole correctness argument, and it
// is why the two probes are in one file rather than beside their receivers.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/map.h"
#include "runtime/namespace.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

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
static Value toStringTagOf(Value objVal, bool& handled) {
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

// Does anything on this receiver's symbol-keyed storage OR the chain above it
// define `sym`? The own half is already probed before this dispatch runs
// (`bronze_elem_get`), so what this adds is the INHERITED half: `class C
// extends B` where `B` defines `static get [Symbol.species]` must reach B's
// accessor rather than the intrinsic answer below, and only a walk can tell.
// Allocates nothing — presence is the shape's answer, and the value is left to
// the ordinary walk that follows.
static bool symbolKeyOnChain(Value objVal, SymbolHeader* sym) {
    ObjectHeader* holder = rtSymbolKeyHolder(objVal);
    const PropertyKey key = PropertyKey::forSymbol(sym);
    for (uint32_t depth = 0; holder && depth < ObjectHeader::kMaxPrototypeDepth; ++depth) {
        PropertyInfo info;
        if (holder->shape && holder->shape->lookupProperty(key, info)) return true;
        holder = holder->protoAncestor(1);
    }
    return false;
}

Value rtWellKnownSymbolMember(Value objVal, Value keyVal, bool& handled) {
    handled = false;
    if (keyVal.asSymbol<SymbolHeader>() == rtSymbolToStringTag()) {
        return toStringTagOf(objVal, handled);
    }
    if (keyVal.asSymbol<SymbolHeader>() == rtSymbolSpecies()) {
        // Every intrinsic that defines one defines it the same way — 23.1.2.5,
        // 24.1.2.2, 24.2.2.2, 25.1.5.4, 27.2.4.6 and 23.2.2.4 are all
        // `get [@@species]() { return this }` — so the answer is the RECEIVER
        // and not the intrinsic. That is what makes a subclass its own species
        // without defining anything: `A extends Array` inherits the accessor
        // through its static chain and `A[@@species]` is `A`.
        //
        // `rtNativeBaseOf` is the "does this constructor inherit one" question
        // already answered for construction, so `Array`, `Map`, `Set`,
        // `Promise` and every class reaching one of them through `extends` are
        // one line rather than five predicates and a chain walk.
        //
        // Through a ROOT, because two of these probes BUILD an intrinsic on
        // first use — `rtIsRegExpConstructor` materializes `RegExp` — and the
        // receiver is read again afterwards, both by the chain walk and by the
        // answer itself. A by-value receiver held across that allocation names
        // a pre-collection address, which is the exact shape of bug the
        // roots-before-the-dispatch comment in `bronze_elem_get` records.
        Rooted<Value> ctor{objVal};
        const bool inheritsSpecies = rtNativeBaseOf(ctor.get()) != NativeBase::None ||
                                     rtIsArrayBufferConstructor(ctor.get()) ||
                                     rtIsTypedArrayConstructor(ctor.get()) ||
                                     rtIsRegExpConstructor(ctor.get());
        if (inheritsSpecies && !symbolKeyOnChain(ctor.get(), rtSymbolSpecies())) {
            handled = true;
            return ctor.get();
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
    if (rtIsMapLike(objVal)) {
        Value props = objVal.asObject<MapHeader>()->properties;
        return props.isObject() ? props.asObject<ObjectHeader>() : nullptr;
    }
    return nullptr;
}

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
Value rtSymbolReadStart(Value v) {
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

}  // namespace bronze::runtime
