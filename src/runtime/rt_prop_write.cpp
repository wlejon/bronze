// The WRITE half of the property path: `o.k = v` and `o[i] = v`, plus the three
// DEFINITION forms a class body and an object literal lower to.
//
// The seam is the one rt_prop_primitive.cpp already took — a receiver kind and
// not a line count — read from the other side. A read asks every receiver the
// same question and each kind answers it from different storage; a write asks
// whether the receiver can hold the property AT ALL, and for most kinds the
// answer is no. So this file is mostly refusals, one per storage story: a typed
// array's named writes have nowhere to go, a Map's entries are not properties, a
// DataView's bytes are written through its accessors. None of them may be
// quietly discarded — a discarded write leaves the program believing it stored
// something, which is the silent-wrong-answer shape the house rules rank below
// process death.
//
// An ARRAY is the receiver that stopped being one of them. It is an object, so
// `a.foo = 1` and `a.length = 0` are ordinary JavaScript; both arms live in
// rt_prop_array.cpp, beside every other path that has to agree about what an
// array owns.
//
// The definition forms are here rather than beside the literal that spells
// them because what separates each from an ordinary assignment is an ATTRIBUTE
// on the write: a class method is `enumerable: false` (15.7.14) and a
// DefineOwnProperty rather than a Set, so a base class's `set m(v)` cannot
// swallow a derived class's `m() {}`. That is a fact about the write path.
//
// The key decoding both halves share — is this key an element index, what
// string does a computed key name — stays in rt_prop.cpp and is reached through
// rt_internal.h, because a key means the same thing in either direction.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/integrity.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/namespace.h"
#include "runtime/regexp.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

// The receiver's kind, for the message of a write that cannot be performed.
// Not `typeof`'s answer: this only ever names a primitive, and it is a
// diagnostic rather than an operator, so it does not want typeof's rooted
// string table.
static const char* primitiveTypeName(Value v) {
    if (v.isString()) return "a string";
    if (v.isNumber()) return "a number";
    if (v.isBool()) return "a boolean";
    return "this value";
}

// The three ways ECMA-262 10.1.9.2 answers false, turned into the TypeError
// 13.15.2 PutValue step 6.d raises for a STRICT reference — and into nothing at
// all for a sloppy one, which is the answer `cases/accessor_properties` pins.
// One place, because `prop.set` and `elem.set` differ only in how they spell
// the key and must not differ in what they do with a refusal.
static void rtReportSetRefusal(SetRefusal refusal, bool strict, const std::string& key) {
    if (!strict || refusal == SetRefusal::None) return;
    switch (refusal) {
        case SetRefusal::NoSetter:
            rtThrowTypeError("Cannot set property '" + key +
                             "' of an object that has only a getter");
            return;
        case SetRefusal::NotWritable:
            rtThrowTypeError("Cannot assign to read only property '" + key + "'");
            return;
        case SetRefusal::NotExtensible:
            rtThrowTypeError("Cannot add property '" + key +
                             "' to an object that is not extensible");
            return;
        case SetRefusal::None:
            return;
    }
}

bool rtStringDataWriteRefused(Value stringData, const std::string& key, bool strict) {
    if (!rtStringDataHasOwnKey(stringData, key)) return false;
    if (strict) {
        rtThrowTypeError("Cannot assign to read only property '" + key + "' of a String object");
    }
    return true;
}

// The same question asked of the WRAPPER rather than of the characters, which
// is the form the two assignment paths below have: they hold a receiver, and
// only a String exotic object among plain objects can refuse a write for a
// reason its shape does not record.
static bool stringExoticRefusesWrite(Value objVal, const std::string& key, bool strict) {
    Value data;
    if (!rtStringWrapperData(objVal, data)) return false;
    return rtStringDataWriteRefused(data, key, strict);
}

extern "C" {

void bronze_prop_set(uint64_t objBits, uint32_t keyIndex, uint64_t valBits, uint64_t* icEntry,
                     bool strict) {
    recordPropCall("bronze_prop_set", keyIndex, icEntry);
    Value objVal(objBits);
    Value valVal(valBits);
    InlineCache* ic = rtAsCache(icEntry);
    // Writing a property of null or undefined is the same TypeError as
    // reading one (ECMA-262 7.3.4), and discarding the write is worse than
    // reading `undefined`: the program believes it stored something.
    if (objVal.isNull() || objVal.isUndefined()) {
        rtThrowTypeError("Cannot set properties of " +
                         std::string(objVal.isNull() ? "null" : "undefined") + " (setting '" +
                         rtKeyString(keyIndex) + "')");
        return;
    }
    // A write to a property of a primitive. 6.2.5.6 PutValue throws for a
    // STRICT reference and discards for a sloppy one, and bronze throws for
    // both — deliberately, and not because `strict` is unavailable here: it is
    // a parameter now. The two lines above have just said that discarding a
    // write is worse than answering `undefined`, and a receiver that can never
    // hold the property is the case where that is most true. It is the one
    // place strict and sloppy are answered the same way on purpose; the house
    // rule prefers the loud answer to the silent one.
    if (!objVal.isObject()) {
        rtThrowTypeError("Cannot create property '" + rtKeyString(keyIndex) +
                         "' on " + primitiveTypeName(objVal));
        return;
    }

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();

    // IC-hit fast path: a shape match writes the slot with no key and no
    // rooting (nothing below can allocate). Writes are NOT inlined into
    // generated code: a write can transition the shape and grow the overflow
    // block, so the interesting half of the work is the miss, and the miss is
    // a call either way (inlines the read).
    if (hdr->flags == HeapKind::Plain && ic && ic->cached_shape) {
        auto* fastObj = reinterpret_cast<ObjectHeader*>(hdr);
        if (ic->describesOwn(fastObj->shape)) {
            if (ic->cached_slot < ObjectHeader::kInlineSlots) {
                fastObj->slotsData()[ic->cached_slot] = valVal;
            } else {
                fastObj->setSlot(ic->cached_slot, valVal);
            }
            return;
        }
    }

    const std::string& keyStr = rtKeyString(keyIndex);
    uint32_t idx = 0;

    if (hdr->flags == HeapKind::Array) {
        // Numeric keys store an element; everything else is one of the two
        // properties an array has beside them, and rt_prop_array.cpp owns both.
        // The index test is FIRST because `a[i] = v` is the hot write and must
        // not walk two string compares to reach the block.
        if (!rtKeyAsIndex(keyStr, idx)) {
            Rooted<Value> arrRoot{objVal};
            if (keyStr == "length") {
                rtReportSetRefusal(rtArraySetLength(arrRoot, valVal), strict, keyStr);
                return;
            }
            StringHeader* named = rtKeyHeader(keyIndex);
            if (!named) fatal("property write with an unregistered key index");
            Rooted<Value> val{valVal};
            Rooted<Value> key(Value::fromString(named));
            rtReportSetRefusal(rtArrayNamedSet(arrRoot, key, val), strict, keyStr);
            return;
        }
        // A frozen or non-extensible array refuses the write on exactly the
        // terms a plain object's property does, so it reports through the same
        // enum and the same strict-mode translation (integrity.h).
        const SetRefusal refusal = rtArrayElementWriteRefusal(objVal, idx);
        if (refusal != SetRefusal::None) {
            rtReportSetRefusal(refusal, strict, keyStr);
            return;
        }
        Rooted<Value> val(valVal);
        reinterpret_cast<ArrayHeader*>(hdr)->setElem(rtHeap(), idx, val);
        return;
    }
    if (hdr->flags == TypedArrayHeader::kFlags) {
        if (!rtKeyAsIndex(keyStr, idx)) {
            fatal(("named property writes on a typed array (" +
                   std::string(reinterpret_cast<TypedArrayHeader*>(hdr)->kindName()) +
                   ") are unsupported").c_str());
        }
        // ToNumber BEFORE the bounds test, because 10.4.5.5
        // IntegerIndexedElementSet performs it whether or not the index is in
        // range. `hdr` survives the call only because rtToNumber cannot
        // allocate: an object is either a named error or a primitive wrapper,
        // and unwrapping one reads an internal slot rather than running
        // ToPrimitive. So nothing here can move the view.
        const double num = rtToNumber(Value(valBits));
        auto* view = reinterpret_cast<TypedArrayHeader*>(hdr);
        if (idx < view->length) view->set(idx, num);
        return;  // out-of-bounds typed-array writes are discarded, per spec
    }
    if (hdr->flags == ArrayBufferHeader::kFlags) {
        fatal("property writes on an ArrayBuffer are unsupported");
    }
    if (hdr->flags == DataViewHeader::kFlags) {
        // 25.3 gives a DataView no writable property and no indexed access —
        // its bytes are reached through `setUint8` and its siblings — and there
        // is no shape here for a named one to go in. Diagnosed rather than
        // discarded, which is what would leave a program believing it stored
        // something.
        fatal(("named property writes on a DataView are unsupported (its bytes are written "
               "through setInt8/setFloat64 and the rest; tried to write `" + keyStr + "`)")
                  .c_str());
    }
    if (hdr->flags == RegExpHeader::kFlags) {
        // `lastIndex` is the one writable property a RegExp has (22.2.6.9).
        // Anything else would need a shape, and discarding the write would
        // leave the program believing it stored something.
        if (!rtRegExpSetMember(objVal, keyStr, valVal)) {
            fatal(("named property writes on a RegExp are unsupported (only `lastIndex` is "
                   "writable; tried to write `" + keyStr + "`)")
                      .c_str());
        }
        return;
    }
    if (hdr->flags == MapHeader::kMapFlags || hdr->flags == MapHeader::kSetFlags) {
        // A Map's entries are reached by `set`/`get`, never by a property
        // write — and there is nowhere to put a named one, since a Map has no
        // shape. Diagnosing is what keeps `m.foo = 1` from being discarded.
        fatal("named property writes on a Map or a Set are unsupported "
              "(use .set(key, value); a Map's keys are not properties)");
    }
    if (hdr->flags == MapHeader::kWeakMapFlags || hdr->flags == MapHeader::kWeakSetFlags) {
        // The same refusal for the same storage: a WeakMap's entries are not
        // properties, and it has no shape for a named one either.
        fatal("named property writes on a WeakMap or a WeakSet are unsupported "
              "(use .set(key, value) / .add(value); its entries are not properties)");
    }
    if (hdr->flags == IterRecordHeader::kFlags) {
        fatal("internal: a property write on an iteration record");
    }
    // 10.4.6.9 [[Set]] returns false for every key, exported or not — so this
    // is a refusal and never a store, and it is a THROW rather than a `fatal`
    // because the specification names the error: strict code catches it.
    if (rtModuleNamespaceWriteRefused(objVal, keyStr, strict)) return;
    if (hdr->flags == HeapKind::Function) {
        // `length` and `name` are own properties of every function and are
        // NON-WRITABLE (10.2.10, 10.2.9), so `f.name = "x"` is discarded in
        // sloppy code and a TypeError in strict — never a store. Without this
        // the write would land in the statics table, which the read consults
        // first, and the program would see a `name` the language says it cannot
        // set. `Object.defineProperty(f, 'name', ...)` is a different operation
        // (they are configurable) and is still refused by kind.
        if (reinterpret_cast<FunctionHeader*>(hdr)->name != nullptr &&
            (keyStr == "length" || keyStr == "name")) {
            rtReportSetRefusal(SetRefusal::NotWritable, strict, keyStr);
            return;
        }
        if (keyStr != "prototype") {
            // A static member: an own property of the function object itself.
            Rooted<Value> fnRoot{objVal};
            Rooted<Value> val{valVal};
            rtEnsureFunctionProperties(fnRoot);
            Rooted<Value> propsRoot{fnRoot.get().asObject<FunctionHeader>()->properties};
            Rooted<Value> key(Value::fromString(rtKeyHeader(keyIndex)));
            SetRefusal refusal = SetRefusal::None;
            propsRoot.get().asObject<ObjectHeader>()->setProp(
                rtHeap(), rtArena(), key, val, /*ic=*/nullptr, /*enumerable=*/true,
                /*defineOwn=*/false, fnRoot.slot_ptr(), &refusal);
            rtReportSetRefusal(refusal, strict, keyStr);
            return;
        }
        // `prototype` is a real own property of the function (10.2.4), so a
        // frozen function refuses a write to it — and it lives in a slot rather
        // than in the statics table, so nothing that table records can answer
        // for it.
        if (!rtFunctionPrototypeWritable(objVal)) {
            rtReportSetRefusal(SetRefusal::NotWritable, strict, keyStr);
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

    // The last receiver kind that answers a write from something other than its
    // shape, and the only one that is a plain object: `s.length = 9` and
    // `s[0] = "z"` on a String exotic object are refused, not stored. The IC
    // above cannot have short-circuited one — an entry is filled by `setProp`,
    // which this refusal keeps from ever running for such a key, so no wrapper
    // shape carries an index or `length` for it to have matched.
    if (stringExoticRefusesWrite(objVal, keyStr, strict)) return;

    // The `Array.prototype` OBJECT is a plain object no array's chain runs
    // through (builtin_array.cpp), so a method installed on it would be found
    // by reads of `Array.prototype` and by nothing an array does — a property
    // that exists and changes nothing, which is the silent lie the refusal
    // exists to prevent.
    if (rtIsArrayPrototypeObject(objVal)) {
        fatal(("decorating Array.prototype is unsupported (an array answers its members "
               "beside the value, so a member written here would never be found on one; "
               "tried to write `" + keyStr + "`)")
                  .c_str());
    }

    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("property write with an unregistered key index");
    // Interned arena key: no allocation before the object is dereferenced.
    // setProp itself may still allocate (overflow growth); it re-derives the
    // object through its own root, and this caller's raw objBits is dead after
    // the call, so that is safe.
    Rooted<Value> key(Value::fromString(keyHeader));
    Rooted<Value> val(valVal);
    SetRefusal refusal = SetRefusal::None;
    objVal.asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, ic,
                                             /*enumerable=*/true, /*defineOwn=*/false,
                                             /*receiver=*/nullptr, &refusal);
    rtReportSetRefusal(refusal, strict, keyStr);
}

// A class method, installed on a prototype (or, for a `static`, on the
// constructor's own-property object). Not `bronze_prop_set`: ECMA-262 15.7.14
// defines a method with `enumerable: false`, and an ordinary assignment
// creates an enumerable property. Its own helper rather than a flag on the
// setter because it has no inline cache — a class body runs once, so the site
// is cold by construction, and giving it a cache entry would spend a slot in
// the module's IC table on a write that never repeats.
void bronze_method_def(uint64_t objBits, uint32_t keyIndex, uint64_t valBits) {
    recordPropCall("bronze_method_def", keyIndex, nullptr);
    Value objVal(objBits);
    if (!objVal.isObject()) {
        fatal("internal: a class method defined on a value that is not an object");
    }
    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("class method definition with an unregistered key index");

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    Rooted<Value> val{Value(valBits)};
    if (hdr->flags == HeapKind::Function) {  // a `static` member: an own property of the function
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

// The same definition with a key that is a VALUE — `class C { [e]() {} }`,
// which for the one spelling bronze admits is `[Symbol.iterator]`. Its own
// helper and not `bronze_elem_set`, because the property is `enumerable: false`
// (15.7.14) and an assignment cannot say that; the `static` split above does
// not repeat here because a class body's computed name is only read for an
// instance method.
//
// ToPropertyKey (7.1.19) has already run on the key when it is a string or a
// symbol, which is every key the class grammar bronze reads can produce. A key
// of any other type is a bug in that grammar rather than a program error, so it
// is fatal by name rather than converted.
void bronze_method_def_computed(uint64_t objBits, uint64_t keyBits, uint64_t valBits) {
    recordElemCall("bronze_method_def_computed");
    Value objVal(objBits);
    if (!objVal.isObject() ||
        objVal.asObject<HeapObjectHeader>()->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        fatal("internal: a computed class method defined on a receiver that is not a plain "
              "object");
    }
    Value keyVal(keyBits);
    if (!keyVal.isString() && !keyVal.isSymbol()) {
        fatal("internal: a computed class method name that is neither a string nor a symbol");
    }
    Rooted<Value> objRoot{objVal};
    Rooted<Value> key{keyVal};
    Rooted<Value> val{Value(valBits)};
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
    recordPropCall("bronze_accessor_def", keyIndex, nullptr);
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
    if (hdr->flags == HeapKind::Function) {  // `static get k()`: an own property of the function
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

void bronze_elem_set(uint64_t objBits, uint64_t idxBits, uint64_t valBits, bool strict) {
    recordElemCall("bronze_elem_set");
    Value objVal(objBits);

    // Fast path: numeric index write on an Array or TypedArray without side-effects.
    if (objVal.isObject() && idxBits <= kNumberMaxBits) {
        const double d = std::bit_cast<double>(idxBits);
        const uint32_t idx = static_cast<uint32_t>(d);
        if (d >= 0.0 && static_cast<double>(idx) == d && d <= 4294967294.0) {
            HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
            if (hdr->flags == HeapKind::Array) {
                auto* arr = reinterpret_cast<ArrayHeader*>(hdr);
                if (idx < arr->length && idx < arr->capacity && !arr->properties.isObject()) {
                    arr->elementsData()[idx] = Value(valBits);
                    return;
                }
            } else if (hdr->flags == TypedArrayHeader::kFlags && valBits <= kNumberMaxBits) {
                auto* view = reinterpret_cast<TypedArrayHeader*>(hdr);
                const double num = std::bit_cast<double>(valBits);
                if (idx < view->length) {
                    view->set(idx, num);
                }
                return;  // out-of-bounds typed-array writes are discarded, per spec
            }
        }
    }

    if (Value(idxBits).isSymbol()) {
        if (objVal.isNull() || objVal.isUndefined()) {
            rtThrowTypeError("Cannot set properties of " +
                             std::string(objVal.isNull() ? "null" : "undefined") +
                             " (setting a symbol-keyed property)");
            return;
        }
        Rooted<Value> recv{objVal};
        // Rooted BEFORE the side-object build below, which allocates. The key
        // needs no root — a symbol lives in the arena and never moves — but the
        // VALUE is an ordinary heap value, and rooting it after the allocation
        // would root a pointer the collector had already moved. That is exactly
        // what it did: `f[sym] = "bare"` on a fresh function read back as an
        // unrelated string under BRONZE_GC_STRESS=1.
        Rooted<Value> val{Value(valBits)};
        // A function's side object of statics is built on first demand, so a
        // symbol-keyed write can be the demand — without this, `f[sym] = v` on
        // a function that had never been given a static reached the "no shape"
        // error below and the write was refused for a receiver that can hold
        // one perfectly well.
        if (recv.get().isObject() &&
            recv.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
            rtEnsureFunctionProperties(recv);
        }
        if (ObjectHeader* holder = rtSymbolKeyHolder(recv.get())) {
            Rooted<Value> holderRoot{Value::fromObject(holder)};
            Rooted<Value> key{Value(idxBits)};
            SetRefusal refusal = SetRefusal::None;
            holderRoot.get().asObject<ObjectHeader>()->setProp(
                rtHeap(), rtArena(), key, val, /*ic=*/nullptr, /*enumerable=*/true,
                /*defineOwn=*/false, recv.slot_ptr(), &refusal);
            // A symbol has no spelling a message can quote back — its
            // description is not its identity — so the position is named
            // instead of the key.
            rtReportSetRefusal(refusal, strict, "<symbol>");
            return;
        }
        // A namespace refuses a SYMBOL key on the same terms as a string one:
        // 10.4.6.9 returns false without ever looking at the key.
        if (rtModuleNamespaceWriteRefused(recv.get(), "<symbol>", strict)) return;
        // A receiver with no shape has nowhere to put one, and discarding the
        // write would leave the program believing it stored something. An ARRAY
        // is refused for a different reason from the rest: it HAS storage for
        // an own property now, but only for a string-named one — the two
        // well-known symbols an array answers are answered beside the value
        // (rt_prop.cpp's `wellKnownSymbolMember`), so an own symbol-keyed
        // property would be written where no read could ever shadow them with
        // it.
        fatal("a symbol-keyed property write is only supported on a plain object or a "
              "function (an array holds string-named own properties only; a Map, a Set "
              "and a typed array carry no shape at all)");
    }
    // A write through `o[i]` to something that is not an object, answered
    // exactly as `bronze_prop_set` answers `o.k` — the same two TypeErrors, in
    // the same order. They are one operation with two spellings, and the read
    // side has already been made to agree (`cases/string_index`); a `fatal`
    // here would kill a process where the `o.k` spelling of the same write is
    // a value a `catch` can hold.
    if (!objVal.isObject()) {
        Rooted<Value> recv{objVal};
        Rooted<Value> key{rtElemKeyAsString(Value(idxBits))};
        const std::string keyText = rtUtf8Chars(key.get().asString<StringHeader>());
        if (recv.get().isNull() || recv.get().isUndefined()) {
            rtThrowTypeError("Cannot set properties of " +
                             std::string(recv.get().isNull() ? "null" : "undefined") +
                             " (setting '" + keyText + "')");
            return;
        }
        rtThrowTypeError("Cannot create property '" + keyText + "' on " +
                         primitiveTypeName(recv.get()));
        return;
    }
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    uint32_t idx = 0;
    if (hdr->flags == HeapKind::Array) {
        if (!rtValueToElementIndex(Value(idxBits), idx)) {
            // A key that is not a canonical array index NAMES a property, and
            // `a[k] = v` means exactly what `a.k = v` means — so it takes the
            // same two arms, through the same file, rather than keeping a
            // second opinion about what `a["length"]` is.
            Rooted<Value> arrRoot{objVal};
            Rooted<Value> val{Value(valBits)};
            Rooted<Value> key{rtElemKeyAsString(Value(idxBits))};
            const std::string keyText = rtUtf8Chars(key.get().asString<StringHeader>());
            if (keyText == "length") {
                rtReportSetRefusal(rtArraySetLength(arrRoot, val.get()), strict, keyText);
                return;
            }
            rtReportSetRefusal(rtArrayNamedSet(arrRoot, key, val), strict, keyText);
            return;
        }
        const SetRefusal refusal = rtArrayElementWriteRefusal(objVal, idx);
        if (refusal != SetRefusal::None) {
            rtReportSetRefusal(refusal, strict, std::to_string(idx));
            return;
        }
        Rooted<Value> val{Value(valBits)};
        reinterpret_cast<ArrayHeader*>(hdr)->setElem(rtHeap(), idx, val);
        return;
    }
    if (hdr->flags == TypedArrayHeader::kFlags) {
        if (!rtValueToElementIndex(Value(idxBits), idx)) {
            fatal(("named property writes on a typed array (" +
                   std::string(reinterpret_cast<TypedArrayHeader*>(hdr)->kindName()) +
                   ") are unsupported").c_str());
        }
        // rtToNumber cannot allocate (see bronze_prop_set), so `hdr` is live.
        const double num = rtToNumber(Value(valBits));
        auto* view = reinterpret_cast<TypedArrayHeader*>(hdr);
        if (idx < view->length) view->set(idx, num);
        return;  // out-of-bounds typed-array writes are discarded, per spec
    }
    if (hdr->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        Rooted<Value> objRoot{objVal};
        Rooted<Value> val{Value(valBits)};
        Rooted<Value> key{rtElemKeyAsString(Value(idxBits))};
        const std::string keyText = rtUtf8Chars(key.get().asString<StringHeader>());
        // `s[0] = "z"`, the spelling this bug actually arrives in. Same answer
        // as `bronze_prop_set`'s, for the reason the two TypeErrors above are
        // the same: one operation, two spellings.
        if (stringExoticRefusesWrite(objRoot.get(), keyText, strict)) return;
        SetRefusal refusal = SetRefusal::None;
        objRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val,
                                                        /*ic=*/nullptr, /*enumerable=*/true,
                                                        /*defineOwn=*/false,
                                                        /*receiver=*/nullptr, &refusal);
        rtReportSetRefusal(refusal, strict, keyText);
        return;
    }
    {
        // `ns[k] = v` is the same operation as `ns.k = v` and gets the same
        // answer, for the reason the two TypeErrors above are shared: one
        // operation, two spellings. The key is only needed for the message.
        Rooted<Value> nsRoot{objVal};
        Rooted<Value> key{rtElemKeyAsString(Value(idxBits))};
        if (rtModuleNamespaceWriteRefused(nsRoot.get(),
                                          rtUtf8Chars(key.get().asString<StringHeader>()),
                                          strict)) {
            return;
        }
    }
    fatal("computed index writes are only supported on arrays, plain objects "
          "and typed arrays");
}

}  // extern "C"

}  // namespace bronze::runtime
