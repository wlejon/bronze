// Array.prototype. Every entry is an ordinary function object over a native
// code pointer (docs/0011 decision 2), handed out by the property path in
// rt_helpers.cpp before it consults the unimplemented-member table.
//
// Two rules govern everything here:
//
//   - Every builtin opens with `RootedArgs args(argc, argv)` and a Rooted
//     `this`. That is the callee half of the calling convention's rooting
//     contract (see rt_internal.h): the incoming block is only as rooted as
//     the caller's frame, so it is copied into roots before anything
//     allocates, and `argv` is never read again.
//   - Anything that allocates can move every object in sight, so the array
//     pointer is RE-DERIVED from the root after every call that can
//     allocate and is never cached across one.
//
// A callback reaches user code through bronze_dynamic_call with a plain
// stack block, which is safe for exactly the reason above: the callee copies
// it before it allocates.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isArray(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == 1;
}

// True when the method may proceed. ECMA-262 defines every Array.prototype
// method over an array-like via ToObject, so a non-array receiver is a
// TypeError — catchable since docs/0020, which is why this reports a verdict
// instead of ending the process.
bool requireArray(Value v, const char* method) {
    if (isArray(v)) return true;
    rtThrowTypeError(std::string("Array.prototype.") + method +
                     " called on a value that is not an array");
    return false;
}

uint32_t lengthOf(Value v) { return v.asObject<ArrayHeader>()->length; }
Value elemOf(Value v, uint32_t i) { return v.asObject<ArrayHeader>()->getElem(i); }

// ECMA-262 ToIntegerOrInfinity: NaN and -0 become 0, everything else
// truncates towards zero.
double toInteger(double d) {
    if (std::isnan(d)) return 0.0;
    if (std::isinf(d)) return d;
    double t = std::trunc(d);
    return t == 0.0 ? 0.0 : t;
}

// A relative index against a length: negative counts back from the end, and
// the result is always within [0, len].
uint32_t relativeIndex(double rel, uint32_t len) {
    if (rel < 0) {
        double from = static_cast<double>(len) + rel;
        return from < 0 ? 0u : static_cast<uint32_t>(from);
    }
    double capped = std::min(rel, static_cast<double>(len));
    return static_cast<uint32_t>(capped);
}

bool sameValueZero(Value a, Value b) {
    // === says NaN is not itself; SameValueZero (what `includes` uses) says
    // it is. That one bit is the whole difference between the two searches.
    if (a.isNumber() && b.isNumber() && std::isnan(a.asNumber()) && std::isnan(b.asNumber())) {
        return true;
    }
    return bronze_strict_eq(a.rawBits(), b.rawBits());
}

Value newArray() {
    ArrayHeader* arr = ArrayHeader::create(rtHeap(), 4);
    arr->header.flags = 1;
    arr->length = 0;
    return Value::fromObject(arr);
}

// Append through the root: growth reallocates the element block and can
// move the array itself.
void appendTo(Rooted<Value>& arrRoot, Rooted<Value>& val) {
    uint32_t at = arrRoot.get().asObject<ArrayHeader>()->length;
    arrRoot.get().asObject<ArrayHeader>()->setElem(rtHeap(), at, val);
}

// One callback invocation: (element, index, array).
Value callBack(Rooted<Value>& fn, Rooted<Value>& thisArg, Rooted<Value>& elem, uint32_t index,
               Rooted<Value>& self) {
    Value block[3] = {elem.get(), Value::fromDouble(static_cast<double>(index)), self.get()};
    return Value(bronze_dynamic_call(fn.get().rawBits(), thisArg.get().rawBits(), 3,
                                     reinterpret_cast<const uint64_t*>(block)));
}

bool requireCallable(Value v, const char* method) {
    if (v.isObject() && v.asObject<HeapObjectHeader>()->flags == 2) return true;
    rtThrowTypeError(std::string("Array.prototype.") + method + " needs a function argument");
    return false;
}

// Whether index `i` is an OWN property of the receiver. `delete a[i]` leaves
// a HOLE (docs/0019 decision 2), and every method ECMA-262 defines in terms
// of HasProperty must skip one rather than visit it as `undefined` — the
// difference between `[1, , 3].forEach(f)` calling `f` twice and calling it
// three times. Deliberately not every method: `find`, `includes`, `join` and
// `at` are defined with Get, so for them a hole IS `undefined` and the loops
// below stay as they are.
bool hasIndex(Value self, uint32_t i) {
    return !isArray(self) || self.asObject<ArrayHeader>()->hasElem(i);
}

// Appends a hole, so a producer that skipped an element still lines its
// output's indices up with its input's — `slice` and `concat` copy the
// absence rather than densifying it.
void appendHole(Rooted<Value>& out) {
    const uint32_t at = out.get().asObject<ArrayHeader>()->length;
    Rooted<Value> filler{Value::fromUndefined()};
    appendTo(out, filler);
    out.get().asObject<ArrayHeader>()->deleteElem(at);
}

// ---- mutators -------------------------------------------------------------

uint64_t arrayPush(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "push")) return Value::fromUndefined().rawBits();
    for (uint32_t i = 0; i < args.count(); ++i) {
        Rooted<Value> val{args[i]};
        appendTo(self, val);
    }
    return Value::fromDouble(lengthOf(self.get())).rawBits();
}

uint64_t arrayPop(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!requireArray(self, "pop")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    if (arr->length == 0) return Value::fromUndefined().rawBits();
    Value last = arr->getElem(arr->length - 1);
    arr->length -= 1;
    return last.rawBits();
}

uint64_t arrayShift(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!requireArray(self, "shift")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    if (arr->length == 0) return Value::fromUndefined().rawBits();
    Value first = arr->getElem(0);
    Value* data = arr->elementsData();
    for (uint32_t i = 1; i < arr->length; ++i) data[i - 1] = data[i];
    arr->length -= 1;
    return first.rawBits();
}

uint64_t arrayUnshift(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "unshift")) return Value::fromUndefined().rawBits();
    const uint32_t n = args.count();
    if (n == 0) return Value::fromDouble(lengthOf(self.get())).rawBits();

    // Every slot the shift needs is made to exist FIRST, one append at a
    // time, so the move below is straight-line code with no allocation in
    // it — an allocation halfway through a memmove would move the block out
    // from under the loop.
    const uint32_t oldLen = lengthOf(self.get());
    for (uint32_t i = 0; i < n; ++i) {
        Rooted<Value> filler{Value::fromUndefined()};
        appendTo(self, filler);
    }
    ArrayHeader* arr = self.get().asObject<ArrayHeader>();
    Value* data = arr->elementsData();
    for (uint32_t i = oldLen; i > 0; --i) data[i - 1 + n] = data[i - 1];
    for (uint32_t i = 0; i < n; ++i) data[i] = args[i];
    return Value::fromDouble(arr->length).rawBits();
}

uint64_t arrayReverse(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!requireArray(self, "reverse")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    Value* data = arr->elementsData();
    for (uint32_t i = 0, j = arr->length; i + 1 < j; ++i, --j) {
        std::swap(data[i], data[j - 1]);
    }
    return self.rawBits();  // reverses IN PLACE and answers the same array
}

uint64_t arrayFill(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self(thisBits);
    if (!requireArray(self, "fill")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    const uint32_t len = arr->length;
    Value fillVal = args[0];
    uint32_t start = args.count() > 1 ? relativeIndex(toInteger(rtToNumber(args[1])), len) : 0;
    uint32_t end = args.count() > 2 && !args[2].isUndefined()
                       ? relativeIndex(toInteger(rtToNumber(args[2])), len)
                       : len;
    Value* data = arr->elementsData();
    for (uint32_t i = start; i < end; ++i) data[i] = fillVal;
    return self.rawBits();
}

// ---- searches -------------------------------------------------------------

uint64_t arrayIndexOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self(thisBits);
    if (!requireArray(self, "indexOf")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    Value needle = args[0];
    uint32_t from =
        args.count() > 1 ? relativeIndex(toInteger(rtToNumber(args[1])), arr->length) : 0;
    for (uint32_t i = from; i < arr->length; ++i) {
        if (!arr->hasElem(i)) continue;
        if (bronze_strict_eq(arr->getElem(i).rawBits(), needle.rawBits())) {
            return Value::fromDouble(i).rawBits();
        }
    }
    return Value::fromDouble(-1.0).rawBits();
}

uint64_t arrayLastIndexOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self(thisBits);
    if (!requireArray(self, "lastIndexOf")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    Value needle = args[0];
    for (uint32_t i = arr->length; i > 0; --i) {
        if (!arr->hasElem(i - 1)) continue;
        if (bronze_strict_eq(arr->getElem(i - 1).rawBits(), needle.rawBits())) {
            return Value::fromDouble(i - 1).rawBits();
        }
    }
    return Value::fromDouble(-1.0).rawBits();
}

uint64_t arrayIncludes(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self(thisBits);
    if (!requireArray(self, "includes")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    for (uint32_t i = 0; i < arr->length; ++i) {
        if (sameValueZero(arr->getElem(i), args[0])) return Value::fromBool(true).rawBits();
    }
    return Value::fromBool(false).rawBits();
}

uint64_t arrayAt(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self(thisBits);
    if (!requireArray(self, "at")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    double rel = toInteger(rtToNumber(args.at(0, Value::fromDouble(0.0))));
    double idx = rel < 0 ? static_cast<double>(arr->length) + rel : rel;
    if (idx < 0 || idx >= static_cast<double>(arr->length)) {
        return Value::fromUndefined().rawBits();
    }
    return arr->getElem(static_cast<uint32_t>(idx)).rawBits();
}

// ---- producers ------------------------------------------------------------

uint64_t arraySlice(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "slice")) return Value::fromUndefined().rawBits();
    const uint32_t len = lengthOf(self.get());
    uint32_t start = args.count() > 0 ? relativeIndex(toInteger(rtToNumber(args[0])), len) : 0;
    uint32_t end = args.count() > 1 && !args[1].isUndefined()
                       ? relativeIndex(toInteger(rtToNumber(args[1])), len)
                       : len;
    Rooted<Value> out{newArray()};
    for (uint32_t i = start; i < end; ++i) {
        if (!hasIndex(self.get(), i)) {
            appendHole(out);
            continue;
        }
        Rooted<Value> elem{elemOf(self.get(), i)};
        appendTo(out, elem);
    }
    return out.get().rawBits();
}

uint64_t arrayConcat(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "concat")) return Value::fromUndefined().rawBits();
    Rooted<Value> out{newArray()};
    for (uint32_t i = 0; i < lengthOf(self.get()); ++i) {
        if (!hasIndex(self.get(), i)) {
            appendHole(out);
            continue;
        }
        Rooted<Value> elem{elemOf(self.get(), i)};
        appendTo(out, elem);
    }
    for (uint32_t a = 0; a < args.count(); ++a) {
        Rooted<Value> arg{args[a]};
        if (!isArray(arg.get())) {
            appendTo(out, arg);
            continue;
        }
        // One level of spreading, which is all concat does — an array
        // nested inside an argument array stays nested.
        for (uint32_t i = 0; i < lengthOf(arg.get()); ++i) {
            if (!hasIndex(arg.get(), i)) {
                appendHole(out);
                continue;
            }
            Rooted<Value> elem{elemOf(arg.get(), i)};
            appendTo(out, elem);
        }
    }
    return out.get().rawBits();
}

uint64_t arrayJoin(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "join")) return Value::fromUndefined().rawBits();
    Rooted<Value> sep{args[0].isUndefined() ? rtMakeString(",") : rtValueToString(args[0])};
    Rooted<Value> acc{rtMakeString("")};

    const uint32_t len = lengthOf(self.get());
    for (uint32_t i = 0; i < len; ++i) {
        if (i > 0) acc.set(StringHeader::concat(rtHeap(), acc, sep));
        Rooted<Value> elem{elemOf(self.get(), i)};
        // null and undefined join as the empty string, not as their
        // ToString — the one place join is not ToString(element).
        if (elem.get().isNull() || elem.get().isUndefined()) continue;
        Rooted<Value> piece{rtValueToString(elem.get())};
        acc.set(StringHeader::concat(rtHeap(), acc, piece));
    }
    return acc.get().rawBits();
}

// ---- iteration ------------------------------------------------------------

uint64_t arrayForEach(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "forEach")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "forEach")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};
    const uint32_t len = lengthOf(self.get());
    for (uint32_t i = 0; i < len; ++i) {
        if (!hasIndex(self.get(), i)) continue;
        Rooted<Value> elem{elemOf(self.get(), i)};
        callBack(fn, thisArg, elem, i, self);
        // The callback is user code and may have thrown. No generated check
        // runs inside this loop, so visiting the next element would be the
        // runtime carrying on past an exception (docs/0020 decision 6).
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    return Value::fromUndefined().rawBits();
}

uint64_t arrayMap(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "map")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "map")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};
    Rooted<Value> out{newArray()};
    const uint32_t len = lengthOf(self.get());
    for (uint32_t i = 0; i < len; ++i) {
        // A hole is not mapped, and the result keeps it: `map` is the one
        // producer whose output length is its input's by definition.
        if (!hasIndex(self.get(), i)) {
            appendHole(out);
            continue;
        }
        Rooted<Value> elem{elemOf(self.get(), i)};
        Rooted<Value> mapped{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        appendTo(out, mapped);
    }
    return out.get().rawBits();
}

uint64_t arrayFilter(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "filter")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "filter")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};
    Rooted<Value> out{newArray()};
    const uint32_t len = lengthOf(self.get());
    for (uint32_t i = 0; i < len; ++i) {
        // `filter` DENSIFIES: a hole is never tested, and nothing is emitted
        // for it, so the result has no holes even when the input did.
        if (!hasIndex(self.get(), i)) continue;
        Rooted<Value> elem{elemOf(self.get(), i)};
        Rooted<Value> kept{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (bronze_truthy(kept.get().rawBits())) {
            appendTo(out, elem);
        }
    }
    return out.get().rawBits();
}

uint64_t arraySome(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "some")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "some")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};
    const uint32_t len = lengthOf(self.get());
    for (uint32_t i = 0; i < len; ++i) {
        if (!hasIndex(self.get(), i)) continue;
        Rooted<Value> elem{elemOf(self.get(), i)};
        Rooted<Value> hit{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (bronze_truthy(hit.get().rawBits())) {
            return Value::fromBool(true).rawBits();
        }
    }
    return Value::fromBool(false).rawBits();
}

uint64_t arrayEvery(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "every")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "every")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};
    const uint32_t len = lengthOf(self.get());
    for (uint32_t i = 0; i < len; ++i) {
        if (!hasIndex(self.get(), i)) continue;
        Rooted<Value> elem{elemOf(self.get(), i)};
        Rooted<Value> held{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (!bronze_truthy(held.get().rawBits())) {
            return Value::fromBool(false).rawBits();
        }
    }
    return Value::fromBool(true).rawBits();
}

// find / findIndex / findLast / findLastIndex differ only in direction and
// in which half of the pair they answer with.
template <bool Reverse, bool WantIndex>
uint64_t arrayFindImpl(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "find")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "find")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};
    const uint32_t len = lengthOf(self.get());
    for (uint32_t n = 0; n < len; ++n) {
        uint32_t i = Reverse ? len - 1 - n : n;
        Rooted<Value> elem{elemOf(self.get(), i)};
        Rooted<Value> found{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (bronze_truthy(found.get().rawBits())) {
            return WantIndex ? Value::fromDouble(i).rawBits() : elem.get().rawBits();
        }
    }
    return WantIndex ? Value::fromDouble(-1.0).rawBits() : Value::fromUndefined().rawBits();
}

template <bool Reverse>
uint64_t arrayReduceImpl(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "reduce")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "reduce")) return Value::fromUndefined().rawBits();
    const uint32_t len = lengthOf(self.get());

    Rooted<Value> acc{Value::fromUndefined()};
    uint32_t next = 0;
    if (args.count() > 1) {
        acc.set(args[1]);
    } else {
        // With no initial value the seed is the first element that is
        // PRESENT, not the first index: `[, 1, 2].reduce(f)` seeds with 1.
        // An array of nothing but holes is as empty as one of length zero.
        while (next < len && !hasIndex(self.get(), Reverse ? len - 1 - next : next)) ++next;
        if (next == len) {
            return rtThrowTypeError("Reduce of empty array with no initial value").rawBits();
        }
        acc.set(elemOf(self.get(), Reverse ? len - 1 - next : next));
        next += 1;
    }

    for (uint32_t n = next; n < len; ++n) {
        uint32_t i = Reverse ? len - 1 - n : n;
        if (!hasIndex(self.get(), i)) continue;
        Rooted<Value> elem{elemOf(self.get(), i)};
        // (accumulator, element, index, array) — four arguments, so this
        // one does not go through callBack.
        Value block[4] = {acc.get(), elem.get(), Value::fromDouble(static_cast<double>(i)),
                          self.get()};
        acc.set(Value(bronze_dynamic_call(fn.get().rawBits(), BRONZE_ABI_UNDEFINED_BITS, 4,
                                          reinterpret_cast<const uint64_t*>(block))));
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    return acc.get().rawBits();
}

struct ArrayMethod {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

// Arity is the count a short call is PADDED to with undefined, so the
// variadic entries declare 0 to see their real argc (docs/0011 decision 2).
// It is not the ECMA-262 `length` of these functions, which bronze does not
// expose — `Function.prototype.length` is a diagnosed unimplemented member.
const ArrayMethod kArrayMethods[] = {
    {"at", arrayAt, 1},
    {"concat", arrayConcat, 0},
    {"every", arrayEvery, 1},
    {"fill", arrayFill, 0},
    {"filter", arrayFilter, 1},
    {"find", arrayFindImpl<false, false>, 1},
    {"findIndex", arrayFindImpl<false, true>, 1},
    {"findLast", arrayFindImpl<true, false>, 1},
    {"findLastIndex", arrayFindImpl<true, true>, 1},
    {"forEach", arrayForEach, 1},
    {"includes", arrayIncludes, 1},
    {"indexOf", arrayIndexOf, 1},
    {"join", arrayJoin, 0},
    {"lastIndexOf", arrayLastIndexOf, 1},
    {"map", arrayMap, 1},
    {"pop", arrayPop, 0},
    {"push", arrayPush, 0},
    {"reduce", arrayReduceImpl<false>, 0},
    {"reduceRight", arrayReduceImpl<true>, 0},
    {"reverse", arrayReverse, 0},
    {"shift", arrayShift, 0},
    {"slice", arraySlice, 0},
    {"some", arraySome, 1},
    {"unshift", arrayUnshift, 0},
};

}  // namespace

Value rtArrayMethod(const std::string& key) {
    for (const ArrayMethod& m : kArrayMethods) {
        if (key == m.name) return Value(bronze_function_singleton(m.code, m.arity));
    }
    return Value::fromUndefined();
}

}  // namespace bronze::runtime
