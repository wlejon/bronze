// `%TypedArray%.prototype` — the methods three.js actually calls, plus the
// `Array.prototype`-shaped ones a program needs to walk a view. One
// implementation each, over any element kind: every read and write goes through
// `TypedArrayHeader::get`/`set`, so the nine views share these bodies exactly
// as they share their header.
//
// The rule that governs every function here: the bytes of a view live in a
// buffer the collector MOVES, so a raw `uint8_t*` or a `TypedArrayHeader*` is
// valid only until the next allocation. Every loop below that can allocate —
// the two that call back into user code — re- derives its pointers from a root
// on each step. The ones that cannot are marked as such, because "this one is
// safe" is a claim that needs saying out loud next to code that looks identical
// to the ones that are not.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/number_format.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isTypedArray(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == TypedArrayHeader::kFlags;
}
bool isArray(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Array;
}

// ECMA-262 defines every %TypedArray%.prototype method with a
// ValidateTypedArray step, so a detached `const s = v.set; s(x)` is a
// TypeError rather than a read of whatever `this` happened to be.
bool requireTypedArray(Value v, const char* method) {
    if (isTypedArray(v)) return true;
    rtThrowTypeError(std::string("%TypedArray%.prototype.") + method +
                     " called on a value that is not a typed array");
    return false;
}

uint32_t lengthOf(Value v) { return v.asObject<TypedArrayHeader>()->length; }
double elemOf(Value v, uint32_t i) { return v.asObject<TypedArrayHeader>()->get(i); }
ElementKind kindOf(Value v) { return v.asObject<TypedArrayHeader>()->elementKind(); }

// ToIntegerOrInfinity, the same one builtin_array.cpp needs; kept local
// because the two files agree on it by both quoting the spec, not by sharing
// a header for six lines.
double toInteger(double d) {
    if (std::isnan(d)) return 0.0;
    if (std::isinf(d)) return d;
    const double t = std::trunc(d);
    return t == 0.0 ? 0.0 : t;
}

// A relative index against a length: negative counts back from the end, and
// the result is always within [0, len].
uint32_t relativeIndex(double rel, uint32_t len) {
    if (rel < 0) {
        const double from = static_cast<double>(len) + rel;
        return from < 0 ? 0u : static_cast<uint32_t>(from);
    }
    return static_cast<uint32_t>(std::min(rel, static_cast<double>(len)));
}

// A start/end argument: absent means the default, present means a relative
// index resolved against the length. Every range-taking method here shares it
// so `slice`, `subarray`, `fill` and `copyWithin` cannot disagree about what
// a negative bound means.
void relativeArg(Value v, uint32_t len, uint32_t& out, uint32_t fallback) {
    out = v.isUndefined() ? fallback : relativeIndex(toInteger(rtToNumber(v)), len);
}

// A fresh view of the same element kind and a length of its own — what
// TypedArraySpeciesCreate produces for `slice` and `map` once the species
// machinery is stripped out (bronze has no `Symbol.species`, which is a named
// deliberate divergence rather than something these methods paper over).
Value newViewLike(Value model, uint32_t length) {
    return Value::fromObject(TypedArrayHeader::create(rtHeap(), kindOf(model), length));
}

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// ---- the methods ------------------------------------------------------------

// 23.2.3.26 %TypedArray%.prototype.set. The source may be another typed array
// (of any element kind) or an ordinary array; anything else is a TypeError
// naming it, rather than a silent no-op over a `length` bronze cannot read.
uint64_t taSet(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "set")) return Value::fromUndefined().rawBits();
    Rooted<Value> source{args[0]};

    const double offsetNum = toInteger(args[1].isUndefined() ? 0.0 : rtToNumber(args[1]));
    if (offsetNum < 0) {
        return rtThrowRangeError("offset is out of bounds").rawBits();
    }
    const uint32_t targetLength = lengthOf(self.get());
    const auto offset = static_cast<uint64_t>(offsetNum);

    if (isTypedArray(source.get())) {
        const uint32_t srcLength = lengthOf(source.get());
        if (offset + srcLength > targetLength) {
            return rtThrowRangeError("offset is out of bounds").rawBits();
        }
        // 23.2.3.26.1 step 15 clones the source when both views share a
        // buffer: a forward element-wise copy between two views of DIFFERENT
        // widths over one buffer overwrites source bytes it has not read yet.
        // Reading the whole source first is that clone, and it is unconditional
        // because the comparison it would replace ("same buffer?") is one this
        // could get subtly wrong for a partial overlap.
        std::vector<double> staged(srcLength);
        for (uint32_t i = 0; i < srcLength; ++i) staged[i] = elemOf(source.get(), i);
        auto* view = self.get().asObject<TypedArrayHeader>();
        for (uint32_t i = 0; i < srcLength; ++i) {
            view->set(static_cast<uint32_t>(offset) + i, staged[i]);
        }
        return Value::fromUndefined().rawBits();
    }

    if (!isArray(source.get())) {
        return rtThrowTypeError("%TypedArray%.prototype.set takes a typed array or an array")
            .rawBits();
    }
    const uint32_t srcLength = source.get().asObject<ArrayHeader>()->length;
    if (offset + srcLength > targetLength) {
        return rtThrowRangeError("offset is out of bounds").rawBits();
    }
    for (uint32_t i = 0; i < srcLength; ++i) {
        // rtToNumber can raise, and a raise allocates the Error object, so
        // both sides are re-derived through their roots on every step.
        Rooted<Value> elem{source.get().asObject<ArrayHeader>()->getElem(i)};
        const double v = rtToNumber(elem.get());
        if (rtExceptionPending()) break;
        self.get().asObject<TypedArrayHeader>()->set(static_cast<uint32_t>(offset) + i, v);
    }
    return Value::fromUndefined().rawBits();
}

// 23.2.3.30 subarray — a new view over the SAME buffer. This is the method
// whose whole meaning is aliasing: writes through either view are visible in
// the other, which is what distinguishes it from `slice`.
uint64_t taSubarray(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "subarray")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    uint32_t begin = 0;
    uint32_t end = len;
    relativeArg(args[0], len, begin, 0);
    relativeArg(args[1], len, end, len);
    const uint32_t count = end > begin ? end - begin : 0;

    auto* view = self.get().asObject<TypedArrayHeader>();
    const ElementKind kind = view->elementKind();
    const uint32_t byteOffset = view->byteOffset + begin * view->bytesPerElement();
    Rooted<Value> buffer{view->buffer};
    return Value::fromObject(
               TypedArrayHeader::createOverBuffer(rtHeap(), kind, buffer, byteOffset, count))
        .rawBits();
}

// 23.2.3.27 slice — a new view over a NEW buffer. The copy is what makes this
// the opposite of `subarray`.
uint64_t taSlice(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "slice")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    uint32_t begin = 0;
    uint32_t end = len;
    relativeArg(args[0], len, begin, 0);
    relativeArg(args[1], len, end, len);
    const uint32_t count = end > begin ? end - begin : 0;

    Rooted<Value> out{newViewLike(self.get(), count)};
    for (uint32_t i = 0; i < count; ++i) {
        out.get().asObject<TypedArrayHeader>()->set(i, elemOf(self.get(), begin + i));
    }
    return out.get().rawBits();
}

// 23.2.3.9 fill. ToNumber runs ONCE, before the loop, which is both what the
// spec says (step 3) and what keeps the loop free of anything that allocates.
uint64_t taFill(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "fill")) return Value::fromUndefined().rawBits();

    const double value = rtToNumber(args[0]);
    if (rtExceptionPending()) return self.get().rawBits();
    const uint32_t len = lengthOf(self.get());
    uint32_t begin = 0;
    uint32_t end = len;
    relativeArg(args[1], len, begin, 0);
    relativeArg(args[2], len, end, len);

    auto* view = self.get().asObject<TypedArrayHeader>();
    for (uint32_t i = begin; i < end; ++i) view->set(i, value);
    return self.get().rawBits();
}

// 23.2.3.6 copyWithin — a move WITHIN one buffer, so overlapping ranges must
// behave as though the source were read first. `memmove` is exactly that
// guarantee, and it works here because both ranges have the same element
// width by construction.
uint64_t taCopyWithin(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "copyWithin")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    uint32_t target = 0;
    uint32_t begin = 0;
    uint32_t end = len;
    relativeArg(args[0], len, target, 0);
    relativeArg(args[1], len, begin, 0);
    relativeArg(args[2], len, end, len);

    const uint32_t count = std::min(end > begin ? end - begin : 0u, len - target);
    if (count > 0) {
        auto* view = self.get().asObject<TypedArrayHeader>();
        const uint32_t bpe = view->bytesPerElement();
        uint8_t* base = view->bytes();
        std::memmove(base + static_cast<size_t>(target) * bpe,
                     base + static_cast<size_t>(begin) * bpe,
                     static_cast<size_t>(count) * bpe);
    }
    return self.get().rawBits();
}

// 23.2.3.15 forEach and 23.2.3.21 map. These two are the only methods here that
// call back into user code, so they are the only ones whose loops can collect —
// hence the re-derivation on every step, and the pending-exception test that
// stops the walk at the first throw.
uint64_t taForEach(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "forEach")) return Value::fromUndefined().rawBits();
    Rooted<Value> cb{args[0]};
    if (!isCallable(cb.get())) {
        return rtThrowTypeError("%TypedArray%.prototype.forEach needs a function argument")
            .rawBits();
    }
    Rooted<Value> thisArg{args[1]};
    const uint32_t len = lengthOf(self.get());
    for (uint32_t i = 0; i < len; ++i) {
        Value block[3] = {Value::fromDouble(elemOf(self.get(), i)),
                          Value::fromDouble(static_cast<double>(i)), self.get()};
        cb.get().asObject<FunctionHeader>()->call(thisArg.get(), 3, block);
        if (rtExceptionPending()) break;
    }
    return Value::fromUndefined().rawBits();
}

uint64_t taMap(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "map")) return Value::fromUndefined().rawBits();
    Rooted<Value> cb{args[0]};
    if (!isCallable(cb.get())) {
        return rtThrowTypeError("%TypedArray%.prototype.map needs a function argument").rawBits();
    }
    Rooted<Value> thisArg{args[1]};
    const uint32_t len = lengthOf(self.get());
    Rooted<Value> out{newViewLike(self.get(), len)};
    for (uint32_t i = 0; i < len; ++i) {
        Value block[3] = {Value::fromDouble(elemOf(self.get(), i)),
                          Value::fromDouble(static_cast<double>(i)), self.get()};
        Rooted<Value> mapped{cb.get().asObject<FunctionHeader>()->call(thisArg.get(), 3, block)};
        if (rtExceptionPending()) break;
        // 23.2.3.21 step 6.d writes through Set, which for a typed array is
        // ToNumber and then the element kind's conversion.
        const double v = rtToNumber(mapped.get());
        if (rtExceptionPending()) break;
        out.get().asObject<TypedArrayHeader>()->set(i, v);
    }
    return out.get().rawBits();
}

// 23.2.3.17 indexOf and 23.2.3.16 includes. Every element is a number, so the
// search value is compared as one — and the two differ in exactly the place
// they differ for arrays: `indexOf` uses IsStrictlyEqual, under which NaN is
// not itself, and `includes` uses SameValueZero, under which it is.
uint64_t taIndexOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "indexOf")) return Value::fromUndefined().rawBits();
    const uint32_t len = lengthOf(self.get());
    if (!args[0].isNumber()) return Value::fromDouble(-1).rawBits();
    const double needle = args[0].asNumber();
    uint32_t from = 0;
    relativeArg(args[1], len, from, 0);
    for (uint32_t i = from; i < len; ++i) {
        if (elemOf(self.get(), i) == needle) return Value::fromDouble(i).rawBits();
    }
    return Value::fromDouble(-1).rawBits();
}

uint64_t taIncludes(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "includes")) return Value::fromUndefined().rawBits();
    const uint32_t len = lengthOf(self.get());
    if (!args[0].isNumber()) return Value::fromBool(false).rawBits();
    const double needle = args[0].asNumber();
    const bool needleIsNaN = std::isnan(needle);
    uint32_t from = 0;
    relativeArg(args[1], len, from, 0);
    for (uint32_t i = from; i < len; ++i) {
        const double v = elemOf(self.get(), i);
        if (needleIsNaN ? std::isnan(v) : v == needle) return Value::fromBool(true).rawBits();
    }
    return Value::fromBool(false).rawBits();
}

// 23.2.3.18 join. Every element is a number, so ToString here is
// ToString(Number) — `formatJsNumber`, the same spelling `String(x)` uses and
// deliberately not console.log's inspect form, which would print `-0`.
uint64_t taJoin(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "join")) return Value::fromUndefined().rawBits();

    std::string sep = ",";
    if (!args[0].isUndefined()) {
        Rooted<Value> sepVal{rtValueToString(args[0])};
        sep = rtUtf8Chars(sepVal.get().asString<StringHeader>());
    }
    const uint32_t len = lengthOf(self.get());
    std::string out;
    char buf[64];
    for (uint32_t i = 0; i < len; ++i) {
        if (i) out += sep;
        const size_t n = formatJsNumber(elemOf(self.get(), i), buf);
        out.append(buf, n);
    }
    return rtMakeString(out).rawBits();
}

// ---- the iterator object (23.2.3.34 / 23.2.5.4) -----------------------------
//
// `for-of` and spread never reach this: `rtOpenIterator` recognises a typed
// array and walks it with a cursor, no iterator object at all. What this exists
// for is a program that reads `v[Symbol.iterator]` and drives it by hand, which
// must get the same values.

// The iterator object's INTERNAL SLOTS (23.1.5.1, which 23.2.5.2 gives a typed
// array's iterator too): [[IteratedArrayLike]] and [[ArrayLikeNextIndex]]. Real
// fields on the object rather than properties under a reserved name, so they
// are absent from `getOwnPropertyNames` as well as from `Object.keys`.
Value readSlot(Rooted<Value>& obj, uint32_t slot) {
    return obj.get().asObject<ObjectHeader>()->internalSlot(slot);
}

void writeSlot(Rooted<Value>& obj, uint32_t slot, Value val) {
    obj.get().asObject<ObjectHeader>()->setInternalSlot(slot, val);
}

Value iterResult(Rooted<Value>& value, bool done) {
    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> vk{rtMakeString("value")};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), vk, value);
    Rooted<Value> dk{rtMakeString("done")};
    Rooted<Value> dv{Value::fromBool(done)};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), dk, dv);
    return out.get();
}

uint64_t taIterNext(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    // 23.1.5.1 step 3: a receiver without the internal slots is a TypeError.
    // The brand is also what makes the slot reads below safe — only an object
    // this file created has fields there at all.
    if (!rtIsIteratorObject(self.get(), IteratorProto::Array)) {
        return rtThrowTypeError("next called on an incompatible receiver").rawBits();
    }
    Rooted<Value> target{readSlot(self, ArrayIteratorSlot::IteratedArrayLike)};
    Rooted<Value> none;
    if (!isTypedArray(target.get())) return iterResult(none, true).rawBits();
    const auto at = static_cast<uint32_t>(readSlot(self, ArrayIteratorSlot::NextIndex).asNumber());
    if (at >= lengthOf(target.get())) return iterResult(none, true).rawBits();

    Rooted<Value> produced{Value::fromDouble(elemOf(target.get(), at))};
    writeSlot(self, ArrayIteratorSlot::NextIndex, Value::fromDouble(static_cast<double>(at + 1)));
    return iterResult(produced, false).rawBits();
}

uint64_t taValues(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "[Symbol.iterator]")) {
        return Value::fromUndefined().rawBits();
    }
    // %ArrayIteratorPrototype% (23.1.5.2, shared by a typed array's iterator
    // through 23.2.5.2), which is where the `[Symbol.iterator]` self-hook lives
    // — inherited from %IteratorPrototype% rather than written here, so this
    // object has no own symbol-keyed property at all. One shared root shape, so
    // every typed-array iterator has the same hidden class and the `next` read
    // inside a loop is a monomorphic cache hit.
    Rooted<Value> it{rtNewIteratorObject(IteratorProto::Array)};
    Rooted<Value> nextFn{Value(bronze_function_singleton(taIterNext, 0))};
    Rooted<Value> nk{rtMakeString("next")};
    it.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), nk, nextFn);
    writeSlot(it, ArrayIteratorSlot::IteratedArrayLike, self.get());
    writeSlot(it, ArrayIteratorSlot::NextIndex, Value::fromDouble(0.0));
    return it.get().rawBits();
}

struct Method {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const Method kMethods[] = {
    {"set", taSet, 0},           {"subarray", taSubarray, 0}, {"slice", taSlice, 0},
    {"fill", taFill, 0},         {"copyWithin", taCopyWithin, 0},
    {"forEach", taForEach, 1},   {"map", taMap, 1},           {"indexOf", taIndexOf, 0},
    {"includes", taIncludes, 0}, {"join", taJoin, 0},
};

}  // namespace

// 23.2.3.34 `%TypedArray%.prototype[@@iterator]`, which 23.2.3.32 makes the
// same function object as `values`. Reached by KEY rather than by name now
// that the key is a symbol, so it is handed out here instead of sitting in the
// string table above — where it only ever was because the key used to be one.
Value rtTypedArrayIteratorMethod() { return Value(bronze_function_singleton(taValues, 0)); }

Value rtTypedArrayMethod(const std::string& key) {
    for (const Method& m : kMethods) {
        if (key == m.name) return Value(bronze_function_singleton(m.code, m.arity));
    }
    return Value::fromUndefined();
}

// The same table asked for existence alone, which is what `in` wants: no
// function object is built, so asking does not allocate and the caller's
// header stays good across it.
bool rtTypedArrayHasMethod(const std::string& key) {
    for (const Method& m : kMethods) {
        if (key == m.name) return true;
    }
    return false;
}

}  // namespace bronze::runtime
