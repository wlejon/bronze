// The named WRITE half of the property path (`o.k = v` and `super.k = v`),
// plus the method and accessor DEFINITION forms a class body and an object
// literal lower to; computed element writes (`o[i] = v`) live in
// rt_elem_write.cpp.
//
// The seam is rt_prop_primitive.cpp's (a receiver kind, not a line count) read
// from the other side: a write asks whether the receiver can hold the property
// AT ALL, and for some kinds the answer is no. So part of this file is refusals
// (a regular expression's named writes, a frozen module namespace), never quiet
// discards: a discarded write is the silent wrong answer the house rules rank
// below process death.
//
// An ARRAY is the receiver that stopped being one of them. It is an object, so
// `a.foo = 1` and `a.length = 0` are ordinary JavaScript; both arms live in
// rt_prop_array.cpp, beside every other path that has to agree about what an
// array owns. The byte-store family followed it: a typed array, an ArrayBuffer
// and a DataView open with an ObjectHeader (typed_array.h), so a named write on
// any of them is the ordinary tail — only a typed array's NUMERIC keys are
// special, and 10.4.5.5 answers those (an element store, or a discard for a
// canonical numeric string that is no valid index) before the tail is reached.
//
// The definition forms are here because what separates each from an assignment
// is an ATTRIBUTE of the write: a class method is `enumerable: false` (15.7.14)
// and a DefineOwnProperty, so a base's `set m(v)` cannot swallow `m() {}`.
//
// The key decoding both halves share stays in rt_prop.cpp (via rt_property.h):
// a key means the same thing in either direction.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/accessor.h"
#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/elem_ic.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/integrity.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/ic_log.h"
#include "runtime/shape_census.h"
#include "runtime/namespace.h"
#include "runtime/proxy.h"
#include "runtime/regexp.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_prop_mega.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/tls_block.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

// The three ways ECMA-262 10.1.9.2 answers false, turned into the TypeError
// 13.15.2 PutValue step 6.d raises for a STRICT reference — and into nothing at
// all for a sloppy one, which is the answer `cases/accessor_properties` pins.
// One place, because `prop.set` and `elem.set` differ only in how they spell
// the key and must not differ in what they do with a refusal — and because the
// builtins that spell their copy `Set(o, k, v, true)` raise the same three
// errors out of sloppy code (declared in rt_property.h for them).
void rtReportSetRefusal(SetRefusal refusal, bool strict, const std::string& key) {
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
        case SetRefusal::TrapRefused:
            rtThrowTypeError("'set' on proxy: trap returned falsish for property '" + key + "'");
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
    if (BRONZE_UNLIKELY(g_shapeCensusEnabled)) {
        // At entry, before anything can allocate: the receiver's shape and
        // whether this store ADDS the key (a transition) are both read here.
        censusRecordAccess(CensusKind::PropSet, objBits, keyIndex, 0, icEntry,
                           BRONZE_CENSUS_RET_ADDR(), /*hasValue=*/true, valBits);
    }
    Value objVal(objBits);
    Value valVal(valBits);
    InlineCacheSite* site = rtAsCacheSite(icEntry);
    InlineCache* ic = nullptr;
    // Writing a property of null or undefined is the same TypeError as
    // reading one (ECMA-262 7.3.4), and discarding the write is worse than
    // reading `undefined`: the program believes it stored something.
    if (objVal.isNull() || objVal.isUndefined()) {
        rtThrowTypeError("Cannot set properties of " +
                         std::string(objVal.isNull() ? "null" : "undefined") + " (setting '" +
                         rtKeyString(keyIndex) + "')");
        return;
    }
    // A write to a property of a primitive: `rtPrimitiveWrite`
    // (rt_prop_primitive.cpp, beside the read that asks the same question of
    // the same receiver kind), which is 13.15.2's answer and is the same for
    // both spellings of the write.
    if (!objVal.isObject()) {
        Rooted<Value> recv{objVal};
        Rooted<Value> val{valVal};
        StringHeader* named = rtKeyHeader(keyIndex);
        if (!named) fatal("property write with an unregistered key index");
        Rooted<Value> key{Value::fromString(named)};
        rtPrimitiveWrite(recv, key, rtKeyString(keyIndex), val, strict);
        return;
    }

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags == HeapKind::Array && rtArrayTruncateFast(objVal, keyIndex, valVal)) return;
    // IC-hit fast path: a shape match writes the slot with no key and no
    // rooting (nothing below can allocate).
    //
    // A typed array is the one shape-carrying kind kept OUT of this path: its
    // numeric keys are elements (10.4.5.5), never slots, and an entry filled by
    // a plain object sharing the view's root shape could otherwise answer a
    // `v[0]` store with a slot write. Its arm below sorts the key first.
    // Past the site's BRONZE_ABI_IC_SET_WAYS ways, a full site probes the
    // thread's (shape, key) table (rt_prop_mega.h); a site with an empty way
    // installs directly into the site instead. The RELATCH-th table answer the
    // set stub counts comes here and skips the table, so MRU insertion shifts
    // the active shape into way 0 (bronze_abi_layout.h).
    const Shape* missShape = nullptr;
    if (site && HeapKind::carriesShape(hdr->flags) && hdr->flags != TypedArrayHeader::kFlags) {
        const Shape* shape = reinterpret_cast<ObjectHeader*>(hdr)->shape;
        ic = site->findForSet(shape);
        if (!ic) {
            if (site->ways[BRONZE_ABI_IC_SET_WAYS - 1].isRealShape()) {
                uint64_t& tableAnswers = icEntry[BRONZE_ABI_IC_SET_MEGA_COUNT_WORD];
                InlineCache* mega = tableAnswers < BRONZE_ABI_IC_SET_MEGA_RELATCH
                                        ? rtMegaPropSetProbe(shape, keyIndex)
                                        : nullptr;
                if (mega) {
                    ++tableAnswers;
                    ic = mega;
                } else {
                    tableAnswers = 0;
                    missShape = shape;
                }
            } else {
                missShape = shape;
            }
        }
    }
    if (ic && ic->cached_shape && HeapKind::carriesShape(hdr->flags) &&
        hdr->flags != TypedArrayHeader::kFlags) {
        auto* fastObj = reinterpret_cast<ObjectHeader*>(hdr);
        if (ic->describesOwn(fastObj->shape)) {
            // `setSlot` for both halves now, where the inline arm used to write
            // the slot array directly. It is the same store — the inline arm of
            // `setSlot` is exactly this line — and routing through it is what
            // keeps the representation choke point total: no write path may
            // reach a slot without the shape having been asked (slot_repr.h).
            fastObj->setSlot(ic->cached_slot, valVal);
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
                Rooted<Value> holderRoot{Value::fromObject(holder)};
                Rooted<Value> recv{objVal};
                Rooted<Value> valRoot{valVal};
                Value setter = holderRoot.get().asObject<ObjectHeader>()->getSlot(ic->cached_slot + 1);
                Rooted<Value> fnRoot{setter};
                if (fnRoot.get().isObject() &&
                    fnRoot.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
                    FunctionHeader* fn = fnRoot.get().asObject<FunctionHeader>();
                    if (fn->code && fn->arity == 1) {
                        rtEnterJs(fn->code, fn->env_record.rawBits(), recv.get().rawBits(), 1,
                                  reinterpret_cast<const uint64_t*>(valRoot.slot_ptr()));
                        return;
                    }
                }
                bool noSetter = false;
                callSetter(fnRoot.get(), recv, valRoot, &noSetter);
                if (noSetter) rtReportSetRefusal(SetRefusal::NoSetter, strict, rtKeyString(keyIndex));
                return;
            }
        }
    }

    // Whatever the full store below installs in the site's write ways for the
    // shape the receiver had, recorded on every way out (a setter that throws
    // included: the entry is the site's, whatever the call did).
    struct MegaSetFill {
        const InlineCacheSite* site;
        const Shape* shape;
        uint32_t key;
        ~MegaSetFill() {
            if (shape) rtMegaPropSetFill(site, shape, key);
        }
    } megaFill{site, missShape, keyIndex};

    const KeyInfo& ki = rtKeyInfo(keyIndex);
    const std::string& keyStr = rtKeyString(keyIndex);

    if (hdr->flags == HeapKind::Array) {
        if (ki.isElemIndex) {
            rtArrayIndexSet(objVal, ki.elemIndex, valVal, strict);
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
            // the conversion itself can throw a TypeError.
            Rooted<Value> viewRoot{objVal};
            rtTypedArraySetElement(viewRoot, ki.elemIndex, Value(valBits));
            return;
        }
        // 10.4.5.5 step 1.b: a canonical numeric string that is not a valid
        // index names nothing — the store reports success in both modes and
        // writes nothing, but TypedArraySetElement (10.4.5.16) still converts
        // the VALUE first, user `valueOf` and its throw included.
        if (rtIsCanonicalNumericString(keyStr)) {
            Rooted<Value> viewRoot{objVal};
            rtTypedArraySetElement(viewRoot, kNoTypedArrayIndex, Value(valBits));
            return;
        }
        // Any other name is an ordinary property of an ordinary object: the
        // tail below.
    }
    if (hdr->flags == RegExpHeader::kFlags && keyStr == "lastIndex") {
        // 22.2.4.1: `lastIndex` is an own writable data property held in the
        // header, so the write is OrdinarySet's store of the VALUE as given —
        // ToLength runs when a match reads it (22.2.7.2 step 2), not here. A
        // frozen RegExp's is non-writable, which is the one refusal. Any other
        // name is an ordinary property of the shape the RegExp carries.
        if (rtIntegrityLevel(objVal) == IntegrityLevel::Frozen) {
            rtReportSetRefusal(SetRefusal::NotWritable, strict, keyStr);
            return;
        }
        rtRegExpRestoreLastIndex(objVal, valVal);
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
            rtPrepareFunctionOwnWrite(fnRoot);
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
            rtThrowTypeError("Assigning a non-object to a function's prototype is not supported");
            return;
        }
        auto* fn = reinterpret_cast<FunctionHeader*>(hdr);
        fn->prototype = valVal;
        // Instances made from here on get the new prototype; ones already made
        // keep their shape, and so keep the old one.
        fn->instance_shape = rtNewRootShape(valVal, rtInstanceInlineSlots(fn));
        return;
    }

    // The last receiver kind that answers a write from something other than its
    // shape, and the only one that is a plain object: `s.length = 9` and
    // `s[0] = "z"` on a String exotic object are refused, not stored. The IC
    // above cannot have short-circuited one — an entry is filled by `setProp`,
    // which this refusal keeps from ever running for such a key, so no wrapper
    // shape carries an index or `length` for it to have matched.
    if (stringExoticRefusesWrite(objVal, keyStr, strict)) return;

    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("property write with an unregistered key index");
    // Interned arena key: no allocation before the object is dereferenced.
    // setProp itself may still allocate (overflow growth); it re-derives the
    // object through its own root, and this caller's raw objBits is dead after
    // the call, so that is safe.
    Rooted<Value> key(Value::fromString(keyHeader));
    Rooted<Value> val(valVal);
    SetRefusal refusal = SetRefusal::None;
    objVal.asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, /*ic=*/nullptr,
                                             /*enumerable=*/true, /*defineOwn=*/false,
                                             /*receiver=*/nullptr, &refusal, /*writable=*/true,
                                             /*configurable=*/true, site);
    rtReportSetRefusal(refusal, strict, keyStr);
}

// `super.k = v`. The walk starts at the HOME OBJECT's prototype and the write
// lands on `this` (10.1.9.2 step 2 sends a data write to the RECEIVER), which
// is why the two objects are separate arguments and why there is no inline
// cache: an entry describes one shape and this write has two objects.
//
// The write used to be handed to `setProp` on the PROTOTYPE with `this` passed
// only as the setter's receiver — so a data write created the property on the
// base prototype, where every later instance of the class read back the value
// one instance had written, and the refusal a frozen `this` owed was never
// asked for because the prototype was extensible. Both follow from step 2, and
// the algorithm that implements it is the one `Reflect.set` already needed.
void bronze_super_set(uint64_t protoBits, uint32_t keyIndex, uint64_t thisBits,
                      uint64_t valBits, bool strict) {
    recordPropCall("bronze_super_set", keyIndex, nullptr);
    if (BRONZE_UNLIKELY(g_shapeCensusEnabled)) {
        censusRecordAccess(CensusKind::SuperSet, thisBits, keyIndex, 0, nullptr,
                           BRONZE_CENSUS_RET_ADDR(), /*hasValue=*/true, valBits);
    }
    // A plain object for an instance element, the base CONSTRUCTOR for a
    // static one (`static m() { super.k = v; }`) — the OrdinarySet below
    // steps onto a function's statics box itself.
    Value protoVal(protoBits);
    if (protoVal.isNull() || protoVal.isUndefined()) {
        rtThrowTypeError("Cannot set properties of " +
                         std::string(protoVal.isNull() ? "null" : "undefined") +
                         " (setting '" + std::string(rtKeyString(keyIndex)) + "')");
        return;
    }
    if (!protoVal.isObject() ||
        (!HeapKind::carriesShape(protoVal.asObject<HeapObjectHeader>()->flags) &&
         protoVal.asObject<HeapObjectHeader>()->flags != HeapKind::Function)) {
        fatal("internal: super property write on a base whose prototype is not an object");
    }
    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("super property write with an unregistered key index");

    Rooted<Value> receiver{Value(thisBits)};
    Rooted<Value> protoRoot{protoVal};
    Rooted<Value> key{Value::fromString(keyHeader)};
    Rooted<Value> val{Value(valBits)};
    const SetRefusal refusal = rtOrdinarySetWithReceiver(protoRoot, key, val, receiver);
    rtReportSetRefusal(refusal, strict, rtKeyString(keyIndex));
}

void bronze_super_elem_set(uint64_t protoBits, uint64_t keyBits, uint64_t thisBits,
                           uint64_t valBits, bool strict) {
    Value protoVal(protoBits);
    if (!protoVal.isObject()) {
        rtThrowTypeError("Cannot set properties of " +
                         std::string(protoVal.isNull() ? "null" : "undefined"));
        return;
    }
    Rooted<Value> protoRoot{protoVal};
    Rooted<Value> keyRoot{Value(keyBits)};
    Rooted<Value> receiver{Value(thisBits)};
    Rooted<Value> val{Value(valBits)};

    keyRoot.set(rtToPropertyKey(keyRoot));

    const SetRefusal refusal = rtOrdinarySetWithReceiver(protoRoot, keyRoot, val, receiver);
    std::string keyStr = keyRoot.get().isString()
                             ? std::string(rtUtf8Chars(keyRoot.get().asObject<StringHeader>()))
                             : "symbol";
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
        rtThrowTypeError("a class method defined on a value that is not an object");
        return;
    }
    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) {
        rtThrowTypeError("class method definition with an unregistered key index");
        return;
    }

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    Rooted<Value> val{Value(valBits)};
    if (hdr->flags == HeapKind::Function) {  // a `static` member: an own property of the function
        Rooted<Value> fnRoot{objVal};
        rtPrepareFunctionOwnWrite(fnRoot);
        Rooted<Value> propsRoot{fnRoot.get().asObject<FunctionHeader>()->properties};
        Rooted<Value> key(Value::fromString(rtKeyHeader(keyIndex)));
        propsRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val,
                                                          /*ic=*/nullptr,
                                                          /*enumerable=*/false,
                                                          /*defineOwn=*/true);
        return;
    }
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        rtThrowTypeError("a class method defined on a receiver that is not a plain object");
        return;
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
namespace {

// 10.2.9 SetFunctionName for a MethodDefinition whose name is COMPUTED
// (`[k]() {}`, `get [k]() {}`, and `get 4() {}` / `5() {}`, which the parser
// hands over as computed keys): the name is the evaluated key — a symbol's
// as "[description]" — behind the accessor's "get " / "set ". Only known
// here, so the function the closure made carries the parser's placeholder
// until now, and it is an arena string (fn.h). Not bronze's closure: left alone.
void nameComputedMethod(Value fnVal, Value key, const char* prefix) {
    if (!fnVal.isObject() || fnVal.asObject<HeapObjectHeader>()->flags != HeapKind::Function) return;
    std::string name;
    if (key.isSymbol()) {
        const StringHeader* desc = key.asSymbol<SymbolHeader>()->description;
        if (desc) name = "[" + rtUtf8Chars(desc) + "]";
    } else if (key.isString()) {
        name = rtUtf8Chars(key.asString<StringHeader>());
    } else {
        return;
    }
    if (prefix) name = std::string(prefix) + " " + name;
    Rooted<Value> fnRoot{fnVal};
    Rooted<Value> nameVal{rtMakeString(name)};
    fnRoot.get().asObject<FunctionHeader>()->name =
        StringHeader::internToArena(rtArena(), nameVal.get().asString<StringHeader>());
}

}  // namespace

void bronze_method_def_computed(uint64_t objBits, uint64_t keyBits, uint64_t valBits) {
    recordElemCall("bronze_method_def_computed", objBits, keyBits);
    Value objVal(objBits);
    if (!objVal.isObject()) {
        rtThrowTypeError("a computed class method defined on a receiver that is not an object");
        return;
    }
    Rooted<Value> objRoot0{objVal};
    Rooted<Value> val{Value(valBits)};
    Rooted<Value> keyRoot{Value(keyBits)};
    // 7.1.19, whose step 1 can run a user `toString`: the receiver and the
    // method are rooted across it, and the header below is re-derived after.
    keyRoot.set(rtToPropertyKey(keyRoot));
    objVal = objRoot0.get();
    Value keyVal = keyRoot.get();
    Rooted<Value> key{keyVal.isString() || keyVal.isSymbol() ? keyVal : rtElemKeyAsString(keyVal)};
    nameComputedMethod(val.get(), key.get(), nullptr);
    objVal = objRoot0.get();

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags == HeapKind::Function) {
        Rooted<Value> fnRoot{objVal};
        rtPrepareFunctionOwnWrite(fnRoot);
        Rooted<Value> propsRoot{fnRoot.get().asObject<FunctionHeader>()->properties};
        propsRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val,
                                                        /*ic=*/nullptr, /*enumerable=*/false,
                                                        /*defineOwn=*/true);
        return;
    }
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        rtThrowTypeError("a computed class method defined on a receiver that is not a plain object");
        return;
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
        rtThrowTypeError("an accessor defined on a value that is not an object");
        return;
    }
    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) {
        rtThrowTypeError("accessor definition with an unregistered key index");
        return;
    }

    Rooted<Value> getter{Value(getterBits)};
    Rooted<Value> setter{Value(setterBits)};
    Rooted<Value> key(Value::fromString(keyHeader));

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags == HeapKind::Function) {  // `static get k()`: an own property of the function
        Rooted<Value> fnRoot{objVal};
        rtPrepareFunctionOwnWrite(fnRoot);
        Rooted<Value> propsRoot{fnRoot.get().asObject<FunctionHeader>()->properties};
        ObjectHeader::defineAccessor(rtHeap(), rtArena(), propsRoot, key, getter, setter,
                                     enumerable);
        return;
    }
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        rtThrowTypeError(std::string("an accessor property on ") + rtObjectKindName(objVal) +
                         " is unsupported");
        return;
    }
    Rooted<Value> objRoot{objVal};
    ObjectHeader::defineAccessor(rtHeap(), rtArena(), objRoot, key, getter, setter, enumerable);
}

void bronze_accessor_def_computed(uint64_t objBits, uint64_t keyBits, uint64_t getterBits,
                                  uint64_t setterBits, bool enumerable) {
    recordElemCall("bronze_accessor_def_computed", objBits, keyBits);
    Value objVal(objBits);
    if (!objVal.isObject()) {
        rtThrowTypeError("an accessor defined on a value that is not an object");
        return;
    }
    Rooted<Value> objRoot0{objVal};
    Rooted<Value> getter{Value(getterBits)};
    Rooted<Value> setter{Value(setterBits)};
    Rooted<Value> keyRoot{Value(keyBits)};
    // 7.1.19 again, with both halves of the accessor rooted across it.
    keyRoot.set(rtToPropertyKey(keyRoot));
    objVal = objRoot0.get();
    Value keyVal = keyRoot.get();
    Rooted<Value> key{keyVal.isString() || keyVal.isSymbol() ? keyVal : rtElemKeyAsString(keyVal)};
    nameComputedMethod(getter.get(), key.get(), "get");
    nameComputedMethod(setter.get(), key.get(), "set");
    objVal = objRoot0.get();

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags == HeapKind::Function) {
        Rooted<Value> fnRoot{objVal};
        rtPrepareFunctionOwnWrite(fnRoot);
        Rooted<Value> propsRoot{fnRoot.get().asObject<FunctionHeader>()->properties};
        ObjectHeader::defineAccessor(rtHeap(), rtArena(), propsRoot, key, getter, setter,
                                     enumerable);
        return;
    }
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        rtThrowTypeError(std::string("an accessor property on ") + rtObjectKindName(objVal) +
                         " is unsupported");
        return;
    }
    Rooted<Value> objRoot{objVal};
    ObjectHeader::defineAccessor(rtHeap(), rtArena(), objRoot, key, getter, setter, enumerable);
}

}  // extern "C"

}  // namespace bronze::runtime
