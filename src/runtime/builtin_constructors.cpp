// The global constructor OBJECTS: `Array`, `String` and `Boolean`.
//
// This file is about the objects a bare name resolves to, which is a different
// thing from the prototype method tables in builtin_array.cpp and
// builtin_string.cpp — those answer "what can I call on a value?", this one
// answers "what does the identifier `Array` denote?". Keeping them apart is
// what stops one file from having to explain both.
//
// The interning mechanism is the typed arrays', unchanged and deliberately not
// re-invented: lowering's closed provided-globals list resolves the name to
// `global.get "<name>"`, `bronze_global_get` asks `rtGlobalConstructor` for it,
// and the answer is a `bronze_function_singleton` INTERNED ON THE CODE POINTER
// — so the bare name and `x.constructor` are the same object and `===` holds
// between them (`Array.isArray(x) ?...: x.constructor === Array` is ordinary
// JS, and three.js writes both spellings).
//
// One distinct C function per constructor is therefore load-bearing: two
// constructors sharing a body with a kind parameter would intern to ONE object
// and `Array === String` would be true.

#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == 2;
}

Value newEmptyArray() {
    ArrayHeader* arr = ArrayHeader::create(rtHeap(), 4);
    arr->header.flags = 1;
    arr->length = 0;
    return Value::fromObject(arr);
}

// Append through the root: growth reallocates the element block and can move
// the array itself.
void appendTo(Rooted<Value>& arrRoot, Rooted<Value>& val) {
    const uint32_t at = arrRoot.get().asObject<ArrayHeader>()->length;
    arrRoot.get().asObject<ArrayHeader>()->setElem(rtHeap(), at, val);
}

// ---- Array (23.1) -----------------------------------------------------------

// `new Array(n)` — n HOLES, not n undefineds. The difference is observable:
// `new Array(3).forEach(f)` calls `f` zero times, and console.log prints `[ <3
// empty items> ]`. Building it as a dense run of `undefined` would be the
// plausible-but-wrong answer.
Value arrayOfLength(uint32_t n) {
    // A dense array costs eight bytes per element, so a length the spec allows
    // is not thereby a length this heap can hold. Refused BEFORE the
    // allocation, so `std::bad_alloc` never unwinds out of a helper generated
    // code called — the same rule a byte store follows.
    const size_t bytes = static_cast<size_t>(n) * sizeof(Value);
    if (bytes + 64 >= rtHeap().reserved_size() / 2) {
        rtThrowRangeError("Array allocation failed: " + std::to_string(n) +
                          " elements does not fit in the heap");
        return Value::fromUndefined();
    }
    ArrayHeader* arr = ArrayHeader::create(rtHeap(), n > 0 ? n : 4);
    arr->header.flags = 1;
    arr->length = n;
    // Nothing between `create` and here allocates, so the raw pointer is live.
    Value* slots = arr->elementsData();
    for (uint32_t i = 0; i < n; ++i) slots[i] = Value::fromHole();
    return Value::fromObject(arr);
}

uint64_t arrayConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // 23.1.1.1 step 3: ONE argument that is a number is a LENGTH, and every
    // other shape is an element list. So `new Array(3)` has length 3 and
    // `new Array("3")` has length 1, and `new Array(3, 4)` has length 2.
    if (args.count() == 1 && args[0].isNumber()) {
        const double len = args[0].asNumber();
        // Step 3.b: the length has to survive a round trip through a uint32, or
        // it was never an array length. NaN, a negative, a fraction and 2^32
        // all fail this and all raise the same RangeError the specification
        // names.
        if (!(len >= 0.0) || len > 4294967295.0 || std::floor(len) != len) {
            return rtThrowRangeError("Invalid array length").rawBits();
        }
        return arrayOfLength(static_cast<uint32_t>(len)).rawBits();
    }
    Rooted<Value> out{newEmptyArray()};
    for (uint32_t i = 0; i < args.count(); ++i) {
        Rooted<Value> elem{args[i]};
        appendTo(out, elem);
    }
    return out.get().rawBits();
}

// 23.1.2.2. The question is about the KIND of the object, not about anything a
// program can install, which is why it is a flags test and not a property read.
uint64_t arrayIsArray(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const Value v = args[0];
    return Value::fromBool(v.isObject() && v.asObject<HeapObjectHeader>()->flags == 1).rawBits();
}

uint64_t arrayOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> out{newEmptyArray()};
    for (uint32_t i = 0; i < args.count(); ++i) {
        Rooted<Value> elem{args[i]};
        appendTo(out, elem);
    }
    return out.get().rawBits();
}

// Does 23.1.2.1 step 3's GetMethod(items, @@iterator) find something? The fast
// kinds answer yes without a property read at all — `rtOpenIterator` steps an
// array, a string, a typed array, a Map and a Set from a cursor — and anything
// else is asked for the well-known key, because the answer decides between the
// iterator path and the array-like one and getting it wrong turns
// `Array.from(userIterable)` into an empty array.
bool hasIterator(Rooted<Value>& src) {
    if (src.get().isString()) return true;
    if (!src.get().isObject()) return false;
    const uint16_t flags = src.get().asObject<HeapObjectHeader>()->flags;
    if (flags == 1 || flags == TypedArrayHeader::kFlags || flags == MapHeader::kMapFlags ||
        flags == MapHeader::kSetFlags) {
        return true;
    }
    if (flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) return false;
    Rooted<Value> key{rtIteratorKey()};
    const Value method = src.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
    return isCallable(method);
}

// The `length` of an array-like, as 23.1.2.1 step 4.a's LengthOfArrayLike reads
// it. A missing `length` is ToLength(undefined), which is 0 — an empty result
// rather than an error, which is what the specification says and what a
// feature-testing program expects.
uint32_t arrayLikeLength(Rooted<Value>& src) {
    if (!src.get().isObject() ||
        src.get().asObject<HeapObjectHeader>()->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        return 0;
    }
    Rooted<Value> key{rtMakeString("length")};
    const double len = rtToNumber(src.get().asObject<ObjectHeader>()->getProp(rtHeap(), key));
    if (!(len >= 1.0)) return 0;
    return len > 4294967295.0 ? 4294967295u : static_cast<uint32_t>(len);
}

Value callMapper(Rooted<Value>& fn, Rooted<Value>& thisArg, Rooted<Value>& item, uint32_t index) {
    Value block[2] = {item.get(), Value::fromDouble(static_cast<double>(index))};
    return Value(bronze_dynamic_call(fn.get().rawBits(), thisArg.get().rawBits(), 2,
                                     reinterpret_cast<const uint64_t*>(block)));
}

uint64_t arrayFrom(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> src{args[0]};
    Rooted<Value> mapFn{args[1]};
    Rooted<Value> thisArg{args[2]};
    if (src.get().isNull() || src.get().isUndefined()) {
        return rtThrowTypeError("Array.from requires an array-like or iterable object, not " +
                                rtIterableKindName(src.get()))
            .rawBits();
    }
    // Step 2.a: checked BEFORE anything is iterated, so a bad mapper does not
    // half-consume the source first.
    if (!mapFn.get().isUndefined() && !isCallable(mapFn.get())) {
        return rtThrowTypeError("Array.from: the second argument is not a function").rawBits();
    }
    Rooted<Value> out{newEmptyArray()};

    if (hasIterator(src)) {
        Rooted<Value> rec{Value(bronze_iter_open(src.get().rawBits()))};
        if (rtExceptionPending()) return out.get().rawBits();
        uint32_t i = 0;
        while (bronze_iter_step(rec.get().rawBits())) {
            Rooted<Value> item{Value(bronze_iter_value(rec.get().rawBits()))};
            if (!mapFn.get().isUndefined()) {
                item.set(callMapper(mapFn, thisArg, item, i));
                // The mapper is user code. Step 6.e.viii closes the iterator
                // when it throws, and carrying on would be the runtime
                // continuing past an exception.
                if (rtExceptionPending()) {
                    bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
                    return out.get().rawBits();
                }
            }
            appendTo(out, item);
            ++i;
        }
        return out.get().rawBits();
    }

    const uint32_t len = arrayLikeLength(src);
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> item{
            Value(bronze_elem_get(src.get().rawBits(), Value::fromDouble(i).rawBits()))};
        if (rtExceptionPending()) return out.get().rawBits();
        if (!mapFn.get().isUndefined()) {
            item.set(callMapper(mapFn, thisArg, item, i));
            if (rtExceptionPending()) return out.get().rawBits();
        }
        appendTo(out, item);
    }
    return out.get().rawBits();
}

// ---- String (22.1) ----------------------------------------------------------

// 22.1.1.1 as a CONVERSION. Called with `new`, the specification builds a
// String exotic OBJECT; bronze hands back the primitive, because a native
// constructor cannot see NewTarget through the uniform calling convention — the
// same divergence the typed array constructors have, in the other direction.
// `String(x)` — which is what programs actually write, and what three.js writes
// — is exact.
uint64_t stringConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // Step 1: no argument at all is the empty string, which is NOT the same as
    // `String(undefined)` — that one is the text "undefined".
    if (args.count() == 0) return rtMakeString("").rawBits();
    // 22.1.1.1 step 2: called WITHOUT `new`, a symbol argument is the one value
    // `String` does not put through ToString — it answers
    // SymbolDescriptiveString instead. Reading it as a plain conversion would
    // make `String(sym)` throw, which is a wrong answer rather than a missing
    // feature: the specification says this call is how you spell it.
    if (args[0].isSymbol()) return rtMakeString(rtSymbolDescriptiveString(args[0])).rawBits();
    return rtValueToString(args[0]).rawBits();
}

// 22.1.2.1. Each argument is ToUint16, so `String.fromCharCode(65, 66)` is
// "AB" and a value outside the range wraps rather than being clamped or
// rejected — the truncation ECMA-262 7.1.7 specifies.
uint64_t stringFromCharCode(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    std::vector<uint16_t> units;
    units.reserve(args.count());
    for (uint32_t i = 0; i < args.count(); ++i) {
        const double n = rtToNumber(args[i]);
        // ToUint16 (7.1.7): NaN and the infinities are 0, everything else
        // truncates towards zero and takes the low sixteen bits.
        if (std::isnan(n) || std::isinf(n)) {
            units.push_back(0);
            continue;
        }
        const double truncated = std::trunc(n);
        const double wrapped = std::fmod(truncated, 65536.0);
        units.push_back(static_cast<uint16_t>(
            static_cast<int32_t>(wrapped < 0 ? wrapped + 65536.0 : wrapped)));
    }
    return rtStringFromUnits(units).rawBits();
}

// ---- Boolean (20.3) ---------------------------------------------------------

// 20.3.1.1 as a conversion, for the same reason `String` is one: with `new` the
// specification builds a wrapper object and bronze hands back the primitive.
// `Boolean(x)` is exactly ToBoolean, which is the only use a program that is
// not testing wrapper identity ever has for it.
uint64_t booleanConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return Value::fromBool(bronze_truthy(args[0].rawBits())).rawBits();
}

// ---- the registry -----------------------------------------------------------

struct StaticFn {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

// Arity 0 everywhere a builtin is variadic, for the reason a builtin is an
// ordinary function object: `FunctionHeader::arity` is the count a short call
// is PADDED to, so a variadic native declaring 2 would see `Array.of(1)` as
// `(1, undefined)` and produce a two-element array.
const StaticFn kArrayStatics[] = {
    {"from", arrayFrom, 0},
    {"isArray", arrayIsArray, 1},
    {"of", arrayOf, 0},
};

const StaticFn kStringStatics[] = {
    {"fromCharCode", stringFromCharCode, 0},
};

// Real static members of each constructor that bronze has not built. `prototype`
// is on every one of them deliberately: bronze has no `Array.prototype` OBJECT
// (the methods are handed out by the property path beside the value, not found
// on a prototype a program can hold — the value-model chunk
// `cases/blocked/object_intrinsic_prototypes` describes), and the empty object
// a FunctionHeader would otherwise answer with is worse than an error, because
// a method installed on it would be found by nothing.
const char* const kArrayCtorUnimplemented[] = {"fromAsync", "prototype"};
const char* const kStringCtorUnimplemented[] = {"fromCodePoint", "prototype", "raw"};
const char* const kBooleanCtorUnimplemented[] = {"prototype"};

// Real members of `Boolean.prototype` (20.3.3), minus `constructor`, which is
// answered. The whole prototype is these three names, so this table is the
// complete remainder rather than a selection — which is what lets the boolean
// branch of the property path stop falling off its end into `undefined`.
const char* const kBooleanProtoUnimplemented[] = {"toString", "valueOf"};

struct CtorEntry {
    const char* name;
    bronze_fn_code code;
    const StaticFn* statics;
    size_t staticCount;
    const char* const* unimplemented;
    size_t unimplementedCount;
    // Whether `new` on it produces what the program asked for. `Array` does:
    // 23.1.1.1 is ONE operation that reads NewTarget only to pick a prototype,
    // so `Array(x)` and `new Array(x)` build the same array. `String` and
    // `Boolean` do not: with `new` the specification builds a String or Boolean
    // exotic OBJECT, and bronze has no such thing.
    bool constructible;
};

const CtorEntry kCtors[] = {
    {"Array", arrayConstructor, kArrayStatics, std::size(kArrayStatics),
     kArrayCtorUnimplemented, std::size(kArrayCtorUnimplemented), true},
    {"String", stringConstructor, kStringStatics, std::size(kStringStatics),
     kStringCtorUnimplemented, std::size(kStringCtorUnimplemented), false},
    {"Boolean", booleanConstructor, nullptr, 0, kBooleanCtorUnimplemented,
     std::size(kBooleanCtorUnimplemented), false},
};

// Arity 0 for the constructors too, and here it decides an answer rather than
// an optimisation: `new Array(3)` padded to any fixed arity would arrive as
// `(3, undefined)`, take 23.1.1.1's element-list branch and produce `[3,
// undefined]` where the language says three holes.
Value ctorObject(const CtorEntry& entry) {
    return Value(bronze_function_singleton(entry.code, 0));
}

}  // namespace

Value rtGlobalConstructor(const std::string& name) {
    for (const CtorEntry& entry : kCtors) {
        if (name == entry.name) return ctorObject(entry);
    }
    return Value::fromUndefined();
}

Value rtArrayConstructorObject() { return ctorObject(kCtors[0]); }

Value rtStringConstructorObject() { return ctorObject(kCtors[1]); }

Value rtBooleanConstructorObject() { return ctorObject(kCtors[2]); }

Value rtBooleanMember(const std::string& key) {
    // The 10.2.5 back-pointer on a PRIMITIVE, the same branch a string's and a
    // number's live in: 7.3.2 GetV would box, and the box is unobservable for
    // every member that exists, so the member is handed out directly.
    if (key == "constructor") return rtBooleanConstructorObject();
    rtCheckUnimplementedMember("Boolean.prototype", kBooleanProtoUnimplemented,
                               std::size(kBooleanProtoUnimplemented), key);
    // A name Boolean.prototype does not define really is absent, and `undefined`
    // is the language's own answer for that.
    return Value::fromUndefined();
}

const char* rtPrimitiveWrapperConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != 2) return nullptr;
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    for (const CtorEntry& entry : kCtors) {
        if (entry.code == code && !entry.constructible) return entry.name;
    }
    return nullptr;
}

const char* rtIntrinsicConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != 2) return nullptr;
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    for (const CtorEntry& entry : kCtors) {
        if (entry.code == code) return entry.name;
    }
    return nullptr;
}

bool rtIsArrayConstructor(Value fn) {
    return fn.isObject() && fn.asObject<HeapObjectHeader>()->flags == 2 &&
           fn.asObject<FunctionHeader>()->code == kCtors[0].code;
}

bool rtGlobalConstructorMember(Value fn, const std::string& key, Value& out) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != 2) return false;
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    for (const CtorEntry& entry : kCtors) {
        if (entry.code != code) continue;
        for (size_t i = 0; i < entry.staticCount; ++i) {
            if (key == entry.statics[i].name) {
                out = Value(bronze_function_singleton(entry.statics[i].code,
                                                       entry.statics[i].arity));
                return true;
            }
        }
        // Diagnoses and does not return for a name on the list, which is what
        // keeps `Array.prototype` from being answered by the FunctionHeader's
        // own prototype slot further down the property path. Anything else
        // falls through to the ordinary function-member treatment, so
        // `Array.call` stays the named error `Function.prototype` gives it.
        rtCheckUnimplementedMember(entry.name, entry.unimplemented, entry.unimplementedCount,
                                   key);
        return false;
    }
    return false;
}

}  // namespace bronze::runtime
