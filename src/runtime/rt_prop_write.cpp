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
// rt_property.h, because a key means the same thing in either direction.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/accessor.h"
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
#include "runtime/ic_log.h"
#include "runtime/namespace.h"
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
#include "runtime/weak_ref.h"

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
    recordPropSetMiss(objBits, keyIndex, valBits, icEntry, strict);
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
        if (ic->isRealShape() && ic->describes(fastObj->shape) && ic->isAccessor()) {
            uint32_t depth = ic->realDepth();
            ObjectHeader* holder = fastObj;
            if (depth > 0) {
                bool crossedDictionary = false;
                holder = fastObj->cachedProtoHolder(depth, crossedDictionary);
            }
            if (holder) {
                Value setter = holder->getSlot(ic->cached_slot + 1);
                if (setter.isObject() &&
                    setter.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
                    FunctionHeader* fn = setter.asObject<FunctionHeader>();
                    if (fn->code && fn->arity == 1) {
                        uint64_t argBits = valVal.rawBits();
                        fn->code(fn->env_record.rawBits(), objBits, 1, &argBits);
                        return;
                    }
                }
                Rooted<Value> live{objVal};
                Rooted<Value> recv{objVal};
                Rooted<Value> v{valVal};
                bool noSetter = false;
                callSetter(setter, recv, v, &noSetter);
                if (noSetter) rtReportSetRefusal(SetRefusal::NoSetter, strict, rtKeyString(keyIndex));
                return;
            }
        }
    }

    const KeyInfo& ki = rtKeyInfo(keyIndex);
    const std::string& keyStr = rtKeyString(keyIndex);

    if (hdr->flags == HeapKind::Array) {
        if (ki.isElemIndex) {
            const uint32_t idx = ki.elemIndex;
            const SetRefusal refusal = rtArrayElementWriteRefusal(objVal, idx);
            if (refusal != SetRefusal::None) {
                rtReportSetRefusal(refusal, strict, rtKeyString(keyIndex));
                return;
            }
            Rooted<Value> val(valVal);
            if (idx > reinterpret_cast<ArrayHeader*>(hdr)->length) {
                Rooted<Value> arrRoot(objVal);
                reinterpret_cast<ArrayHeader*>(hdr)->setLength(rtHeap(), arrRoot, idx + 1);
                hdr = arrRoot.get().asObject<HeapObjectHeader>();
            }
            reinterpret_cast<ArrayHeader*>(hdr)->setElem(rtHeap(), idx, val);
            return;
        }
        if (ki.isLength) {
            Rooted<Value> arrRoot{objVal};
            rtReportSetRefusal(rtArraySetLength(arrRoot, valVal), strict, "length");
            return;
        }
        StringHeader* named = rtKeyHeader(keyIndex);
        if (!named) fatal("property write with an unregistered key index");
        Rooted<Value> arrRoot{objVal};
        Rooted<Value> val{valVal};
        Rooted<Value> key(Value::fromString(named));
        rtReportSetRefusal(rtArrayNamedSet(arrRoot, key, val), strict, rtKeyString(keyIndex));
        return;
    }
    if (hdr->flags == TypedArrayHeader::kFlags) {
        if (ki.isElemIndex) {
            // 10.4.5.5 runs ToNumber on the VALUE, and for an object that is a
            // user `valueOf` — so the view is reached through a root afterwards
            // rather than through the header taken above, and its length is
            // re-read: a collection during the conversion moves the object, and
            // the conversion itself can leave a TypeError pending.
            Rooted<Value> viewRoot{objVal};
            rtTypedArraySetElement(viewRoot, ki.elemIndex, Value(valBits));
            return;
        }
        fatal(("named property writes on a typed array (" +
               std::string(reinterpret_cast<TypedArrayHeader*>(hdr)->kindName()) +
               ") are unsupported").c_str());
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
    if (hdr->flags == WeakRefHeader::kFlags ||
        hdr->flags == FinalizationRegistryHeader::kFlags) {
        // 26.1.3 and 26.2.3 give neither a writable property, and there is no
        // shape here for a named one to go in. Diagnosed rather than discarded,
        // which is what would leave a program believing it stored something —
        // and the mistake this catches is a real one: a program that means to
        // remember what a WeakRef points at writes it beside the ref.
        fatal((std::string("named property writes on a ") +
               (hdr->flags == WeakRefHeader::kFlags ? "WeakRef" : "FinalizationRegistry") +
               " are unsupported (tried to write `" + keyStr + "`)")
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
    if (rtIsMapLike(objVal)) {
        // 24.1.4: a Map is an ORDINARY object with internal slots, so an
        // ordinary named write defines an ordinary property and leaves
        // [[MapData]] alone. `m.foo = 1` and `m.set("foo", 1)` are two stores
        // that never see each other, which is why this is a property write and
        // not a redirection into the entry table.
        StringHeader* named = rtKeyHeader(keyIndex);
        if (!named) fatal("property write with an unregistered key index");
        Rooted<Value> mapRoot{objVal};
        Rooted<Value> val{valVal};
        Rooted<Value> key(Value::fromString(named));
        rtReportSetRefusal(rtMapNamedSet(mapRoot, key, val), strict, rtKeyString(keyIndex));
        return;
    }
    if (hdr->flags == HeapKind::Proxy) {
        // 10.5.9: the `set` trap, or the target's write through this same
        // funnel — the strict flag rides along so a trap's `false` and a
        // forwarded refusal both surface exactly where a plain write's would.
        StringHeader* named = rtKeyHeader(keyIndex);
        if (!named) fatal("property write with an unregistered key index");
        rtProxySet(objVal, Value::fromString(named), valVal, strict);
        return;
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
    if (!objVal.isObject()) {
        fatal("internal: a computed class method defined on a receiver that is not an object");
    }
    Rooted<Value> objRoot0{objVal};
    Rooted<Value> val{Value(valBits)};
    Rooted<Value> keyRoot{Value(keyBits)};
    // 7.1.19, whose step 1 can run a user `toString`: the receiver and the
    // method are rooted across it, and the header below is re-derived after.
    keyRoot.set(rtToPropertyKey(keyRoot));
    if (rtExceptionPending()) return;
    objVal = objRoot0.get();
    Value keyVal = keyRoot.get();
    Rooted<Value> key{keyVal.isString() || keyVal.isSymbol() ? keyVal : rtElemKeyAsString(keyVal)};

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags == HeapKind::Function) {
        Rooted<Value> fnRoot{objVal};
        rtEnsureFunctionProperties(fnRoot);
        Rooted<Value> propsRoot{fnRoot.get().asObject<FunctionHeader>()->properties};
        propsRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val,
                                                        /*ic=*/nullptr, /*enumerable=*/false,
                                                        /*defineOwn=*/true);
        return;
    }
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        fatal("internal: a computed class method defined on a receiver that is not a plain "
              "object");
    }
    Rooted<Value> objRoot{objVal};
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
        // Every receiver that is not a plain object and not a function, named
        // rather than guessed at: an accessor lives in a shape, and each of
        // these kinds answers its members from a table beside the value with no
        // shape of its own to put a getter/setter pair in. The side property
        // objects an array and a Map now carry are the storage a fix would use,
        // but the READ paths that would have to consult them are per-kind and
        // per-member, so the refusal stands rather than half of it.
        fatal((std::string("an accessor property on ") + rtObjectKindName(objVal) +
               " is unsupported (its members are answered from a table beside the value, not "
               "from a shape an accessor could live in)")
                  .c_str());
    }
    Rooted<Value> objRoot{objVal};
    ObjectHeader::defineAccessor(rtHeap(), rtArena(), objRoot, key, getter, setter, enumerable);
}

void bronze_accessor_def_computed(uint64_t objBits, uint64_t keyBits, uint64_t getterBits,
                                  uint64_t setterBits, bool enumerable) {
    recordElemCall("bronze_accessor_def_computed");
    Value objVal(objBits);
    if (!objVal.isObject()) {
        fatal("internal: an accessor defined on a value that is not an object");
    }
    Rooted<Value> objRoot0{objVal};
    Rooted<Value> getter{Value(getterBits)};
    Rooted<Value> setter{Value(setterBits)};
    Rooted<Value> keyRoot{Value(keyBits)};
    // 7.1.19 again, with both halves of the accessor rooted across it.
    keyRoot.set(rtToPropertyKey(keyRoot));
    if (rtExceptionPending()) return;
    objVal = objRoot0.get();
    Value keyVal = keyRoot.get();
    Rooted<Value> key{keyVal.isString() || keyVal.isSymbol() ? keyVal : rtElemKeyAsString(keyVal)};

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags == HeapKind::Function) {
        Rooted<Value> fnRoot{objVal};
        rtEnsureFunctionProperties(fnRoot);
        Rooted<Value> propsRoot{fnRoot.get().asObject<FunctionHeader>()->properties};
        ObjectHeader::defineAccessor(rtHeap(), rtArena(), propsRoot, key, getter, setter,
                                     enumerable);
        return;
    }
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        // Every receiver that is not a plain object and not a function, named
        // rather than guessed at: an accessor lives in a shape, and each of
        // these kinds answers its members from a table beside the value with no
        // shape of its own to put a getter/setter pair in. The side property
        // objects an array and a Map now carry are the storage a fix would use,
        // but the READ paths that would have to consult them are per-kind and
        // per-member, so the refusal stands rather than half of it.
        fatal((std::string("an accessor property on ") + rtObjectKindName(objVal) +
               " is unsupported (its members are answered from a table beside the value, not "
               "from a shape an accessor could live in)")
                  .c_str());
    }
    Rooted<Value> objRoot{objVal};
    ObjectHeader::defineAccessor(rtHeap(), rtArena(), objRoot, key, getter, setter, enumerable);
}

void bronze_elem_set(uint64_t objBits, uint64_t idxBits, uint64_t valBits, bool strict) {
    recordElemCall("bronze_elem_set");
    Value objVal(objBits);

    // 7.1.19 ToPropertyKey for an OBJECT key, at the entry and nowhere below,
    // for the reason `bronze_elem_get` does it here: the key's `toString` is
    // user code, and every branch past this point holds a raw receiver header
    // across the key conversion. The receiver AND the value are rooted, because
    // the value is just as movable as the receiver and is not touched again
    // until the write itself.
    if (Value(idxBits).isObject()) {
        Rooted<Value> objRoot{objVal};
        Rooted<Value> valRoot{Value(valBits)};
        Rooted<Value> keyRoot{Value(idxBits)};
        keyRoot.set(rtToPropertyKey(keyRoot));
        if (rtExceptionPending()) return;
        bronze_elem_set(objRoot.get().rawBits(), keyRoot.get().rawBits(),
                        valRoot.get().rawBits(), strict);
        return;
    }

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
                // A NUMBER into a BIGINT view is the one store that throws
                // (7.1.13), so this fast path — whose whole premise is that a
                // Number needs no conversion — is not for those two kinds.
                if (isBigIntElementKind(view->elementKind())) {
                    Rooted<Value> viewRoot{objVal};
                    rtTypedArraySetElement(viewRoot, idx, Value(valBits));
                    return;
                }
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
        if (recv.get().isObject() &&
            recv.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
            ArrayHeader::ensureProperties(rtHeap(), rtArena(), recv);
        }
        // And a Map or a Set, on the same demand and for the same reason: its
        // properties are ordinary (24.1.4) and a symbol-keyed one is no
        // different from a named one.
        if (rtIsMapLike(recv.get())) {
            MapHeader::ensureProperties(rtHeap(), rtArena(), recv);
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
        fatal("a symbol-keyed property write is only supported on a plain object, a "
              "function, an array or a Map-like collection (a typed array carries no "
              "shape at all)");
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
    if (hdr->flags == HeapKind::Proxy) {
        // One operation, two spellings, like every arm below: `p[k] = v` is
        // `p.k = v` and takes the same trap-or-forward path.
        rtProxySet(objVal, Value(idxBits), Value(valBits), strict);
        return;
    }
    Value idxVal(idxBits);
    if (idxVal.isNumber()) {
        double d = idxVal.asNumber();
        if (d >= 0.0 && d <= 4294967294.0) {
            uint32_t u = static_cast<uint32_t>(d);
            if (static_cast<double>(u) == d) {
                if (hdr->flags == HeapKind::Array) {
                    const SetRefusal refusal = rtArrayElementWriteRefusal(objVal, u);
                    if (refusal != SetRefusal::None) {
                        rtReportSetRefusal(refusal, strict, std::to_string(u));
                        return;
                    }
                    Rooted<Value> val{Value(valBits)};
                    if (u > reinterpret_cast<ArrayHeader*>(hdr)->length) {
                        Rooted<Value> arrRoot(objVal);
                        reinterpret_cast<ArrayHeader*>(hdr)->setLength(rtHeap(), arrRoot, u + 1);
                        hdr = arrRoot.get().asObject<HeapObjectHeader>();
                    }
                    reinterpret_cast<ArrayHeader*>(hdr)->setElem(rtHeap(), u, val);
                    return;
                }
                if (hdr->flags == TypedArrayHeader::kFlags) {
                    // The conversion (ToNumber, or ToBigInt for the two 64-bit
                    // integer views) can be a user `valueOf`; the funnel
                    // re-derives the view from the root after it.
                    Rooted<Value> viewRoot{objVal};
                    rtTypedArraySetElement(viewRoot, u, Value(valBits));
                    return;
                }
            }
        }
    }
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
        if (idx > reinterpret_cast<ArrayHeader*>(hdr)->length) {
            Rooted<Value> arrRoot(objVal);
            reinterpret_cast<ArrayHeader*>(hdr)->setLength(rtHeap(), arrRoot, idx + 1);
            hdr = arrRoot.get().asObject<HeapObjectHeader>();
        }
        reinterpret_cast<ArrayHeader*>(hdr)->setElem(rtHeap(), idx, val);
        return;
    }
    if (hdr->flags == TypedArrayHeader::kFlags) {
        // A NUMBER key that reaches here failed the exact-uint32 fast path
        // above, so it is NaN, negative, fractional or >= 2^32-1. On an
        // integer-indexed exotic object every number is a NUMERIC index
        // (10.4.5.5 through CanonicalNumericIndexString), and an invalid one
        // is the spec's silently-discarded store — never a named property.
        // Only an actual string key can name one.
        if (Value(idxBits).isNumber()) return;
        if (!rtValueToElementIndex(Value(idxBits), idx)) {
            fatal(("named property writes on a typed array (" +
                   std::string(reinterpret_cast<TypedArrayHeader*>(hdr)->kindName()) +
                   ") are unsupported").c_str());
        }
        Rooted<Value> viewRoot{objVal};
        rtTypedArraySetElement(viewRoot, idx, Value(valBits));
        return;  // out-of-bounds typed-array writes are discarded, per spec
    }
    if (hdr->flags == HeapKind::Function) {
        Rooted<Value> fnRoot{objVal};
        Rooted<Value> val{Value(valBits)};
        Rooted<Value> key{rtElemKeyAsString(Value(idxBits))};
        const std::string keyText = rtUtf8Chars(key.get().asString<StringHeader>());
        if (keyText == "prototype") {
            if (!rtFunctionPrototypeWritable(objVal)) {
                // A REFUSAL, not a diagnosis of the value: 10.1.9.2 discards
                // the write in sloppy code and 13.15.2 throws in strict, and
                // that is true whatever was being assigned. The `fatal` that
                // used to stand here said "assigning a non-object", which was
                // the wrong sentence about the right situation.
                rtReportSetRefusal(SetRefusal::NotWritable, strict, keyText);
                return;
            }
            auto* fn = reinterpret_cast<FunctionHeader*>(hdr);
            fn->prototype = val.get();
            return;
        }
        rtEnsureFunctionProperties(fnRoot);
        Rooted<Value> propsRoot{fnRoot.get().asObject<FunctionHeader>()->properties};
        SetRefusal refusal = SetRefusal::None;
        propsRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val,
                                                         /*ic=*/nullptr, /*enumerable=*/true,
                                                         /*defineOwn=*/false,
                                                         /*receiver=*/nullptr, &refusal);
        rtReportSetRefusal(refusal, strict, keyText);
        return;
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
    if (rtIsMapLike(objVal)) {
        // `m[k] = v` is `m.k = v`: a Map's key space is values and its
        // property space is names, so nothing here is an element and every
        // key names an ordinary property. One operation, two spellings.
        Rooted<Value> mapRoot{objVal};
        Rooted<Value> val{Value(valBits)};
        Rooted<Value> key{rtElemKeyAsString(Value(idxBits))};
        const std::string keyText = rtUtf8Chars(key.get().asString<StringHeader>());
        rtReportSetRefusal(rtMapNamedSet(mapRoot, key, val), strict, keyText);
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
