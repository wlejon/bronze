// The global constructor OBJECTS: `Array`, `String`, `Boolean` and `Number`.
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
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

Value newEmptyArray() {
    ArrayHeader* arr = ArrayHeader::create(rtHeap(), 4);
    arr->header.flags = HeapKind::Array;
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
    arr->header.flags = HeapKind::Array;
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
    return Value::fromBool(v.isObject() &&
                           v.asObject<HeapObjectHeader>()->flags == HeapKind::Array)
        .rawBits();
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
    if (flags == HeapKind::Array || flags == TypedArrayHeader::kFlags ||
        flags == MapHeader::kMapFlags ||
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

// 22.1.1.1 as a CONVERSION, which is the whole of this body. A native
// constructor cannot see NewTarget through the uniform calling convention, so
// the `new` form is not a branch here at all: `bronze_construct` recognises
// this function object and builds the String exotic object instead of ever
// entering it (builtin_wrappers.cpp).
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
    // 7.1.17 ToString, step 1 included: an object is ToPrimitive'd with hint
    // STRING, so `toString` is tried before `valueOf` — the opposite of what
    // `'' + o` does, and observably so for an object that defines both.
    Rooted<Value> value{args[0]};
    return rtToStringValue(value).rawBits();
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

// 20.3.1.1 as a conversion, for the same reason `String` is one: the `new` form
// is intercepted before this body runs. `Boolean(x)` is exactly ToBoolean, which
// is the only use a program that is not testing wrapper identity ever has for
// it.
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

// Real static members of each constructor that bronze has not built.
// `Array.prototype` left this list when it became an object of its own
// (builtin_array.cpp): an array still answers its members BESIDE the value,
// but the VALUE the expression denotes is real now — the same method objects,
// off the same table, with a write to it refused by name so nothing can be
// installed there that no array would find. `String.prototype`,
// `Boolean.prototype` and `Number.prototype` are answered from the
// constructor's own prototype slot.
const char* const kArrayCtorUnimplemented[] = {"fromAsync"};
const char* const kStringCtorUnimplemented[] = {"fromCodePoint", "raw"};

struct CtorEntry {
    const char* name;
    bronze_fn_code code;
    const StaticFn* statics;
    size_t staticCount;
    const char* const* unimplemented;
    size_t unimplementedCount;
    // The intrinsic prototype this constructor's instances have, or null when
    // its instances have no prototype OBJECT at all — which is what separates
    // the two kinds of constructor in this file. `Array` builds a value whose
    // members are answered beside it, so `new` can simply run the body: 23.1.1.1
    // reads NewTarget only to pick a prototype, which is why `Array(x)` and
    // `new Array(x)` agree. `String` and `Boolean` build a WRAPPER, which the
    // body cannot return at all (13.3.5.1 discards a primitive return in favour
    // of the plain instance), so `bronze_construct` builds it from this rather
    // than entering the body.
    Value (*prototype)();
    // Own properties this constructor carries that are not in the table above,
    // installed on the function object the first time anything asks for it.
    // `Number` is the one entry that has any: its fourteen statics are the
    // tables in builtin_number.cpp, and 21.1.2 makes them non-enumerable own
    // properties rather than the beside-the-value answers `Array.from` and
    // `String.fromCharCode` are — so a program can ask `'EPSILON' in Number`
    // and get the language's answer. Idempotent, because every route to a
    // constructor reaches the same interned function object.
    void (*decorate)(Rooted<Value>&);
};

const CtorEntry kCtors[] = {
    {"Array", arrayConstructor, kArrayStatics, std::size(kArrayStatics),
     kArrayCtorUnimplemented, std::size(kArrayCtorUnimplemented), nullptr, nullptr},
    {"String", stringConstructor, kStringStatics, std::size(kStringStatics),
     kStringCtorUnimplemented, std::size(kStringCtorUnimplemented), rtStringPrototype, nullptr},
    {"Boolean", booleanConstructor, nullptr, 0, nullptr, 0, rtBooleanPrototype, nullptr},
    // No unimplemented list: 21.1.2 names fifteen own properties and bronze
    // answers all fifteen (builtin_number.cpp says so at the table).
    {"Number", rtNumberConstructorBody, nullptr, 0, nullptr, 0, rtNumberPrototype,
     rtInstallNumberStatics},
};

// Arity 0 for the constructors too, and here it decides an answer rather than
// an optimisation: `new Array(3)` padded to any fixed arity would arrive as
// `(3, undefined)`, take 23.1.1.1's element-list branch and produce `[3,
// undefined]` where the language says three holes.
//
// The `prototype` slot is filled in on first demand rather than left to
// `rtEnsureFunctionPrototype`, which mints a FRESH empty object for a function
// that has none — and that object is what `String.prototype` would read and
// what `instanceof` would compare against, so a wrapper would not be an
// `instanceof String`. Filling it here makes those one answer.
Value ctorObject(const CtorEntry& entry) {
    Rooted<Value> fn{rtNativeFunction(entry.code, 0)};
    if (entry.prototype && !fn.get().asObject<FunctionHeader>()->prototype.isObject()) {
        Rooted<Value> proto{entry.prototype()};
        FunctionHeader* live = fn.get().asObject<FunctionHeader>();
        live->prototype = proto.get();
        // Memoized on the prototype's identity, so repeating this read cannot
        // leak a shape per call. Nothing ever builds an object from it —
        // `bronze_construct` does not reach the ordinary instance path for
        // these two.
        live->instance_shape = rtRootShapeForPrototype(proto.get());
    }
    if (entry.decorate) entry.decorate(fn);
    return fn.get();
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

Value rtNumberConstructorObject() { return ctorObject(kCtors[3]); }

const char* rtPrimitiveWrapperConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    for (const CtorEntry& entry : kCtors) {
        if (entry.code == code && entry.prototype) return entry.name;
    }
    return nullptr;
}

const char* rtIntrinsicConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    for (const CtorEntry& entry : kCtors) {
        if (entry.code == code) return entry.name;
    }
    return nullptr;
}

bool rtIsArrayConstructor(Value fn) {
    return fn.isObject() && fn.asObject<HeapObjectHeader>()->flags == HeapKind::Function &&
           fn.asObject<FunctionHeader>()->code == kCtors[0].code;
}

bool rtGlobalConstructorMember(Value fn, const std::string& key, Value& out) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return false;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    for (const CtorEntry& entry : kCtors) {
        if (entry.code != code) continue;
        // 22.1.2.3 / 20.3.2.1, ahead of the statics and of the function's own
        // prototype slot further down the property path. The slot holds the
        // same object, so this is about where the answer is WRITTEN rather than
        // what it is: one line here keeps `Array.prototype`'s named refusal
        // legible beside the two that now answer.
        if (entry.prototype && key == "prototype") {
            out = entry.prototype();
            return true;
        }
        // 23.1.3.4's holder, answered HERE and not through the entry's
        // prototype slot: filling that slot would put `Array` on
        // `rtPrimitiveWrapperConstructorName`'s list — it reports any entry
        // with a prototype — and `new Array` would build a wrapper instead of
        // running 23.1.1.1.
        if (key == "prototype" && entry.code == arrayConstructor) {
            out = rtArrayPrototypeObject();
            return true;
        }
        for (size_t i = 0; i < entry.staticCount; ++i) {
            if (key == entry.statics[i].name) {
                out = rtNativeFunction(entry.statics[i].code,
                                                       entry.statics[i].arity);
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
