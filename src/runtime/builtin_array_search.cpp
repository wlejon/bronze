#include "runtime/builtin_array_internal.h"
#include "runtime/native_fast.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"

#include <algorithm>
#include <cmath>

namespace bronze::runtime {

namespace {

// IsStrictlyEqual (7.2.15) of each element of an Array's block in [from, end)
// against `needle`, split by the needle's type so the walk is a compare per
// element: a Number matches a Number of the same value (NaN nothing, so the
// walk is skipped); a String or a BigInt asks bronze_strict_eq only of
// elements of its own tag; anything else is equal exactly when the bits are.
// A hole is a tag no needle carries, so it matches nothing and needs no test
// of its own. Returns the index, or -1.
//
// A needle that is no String or BigInt compares without a call, so nothing
// here allocates and the block may be walked through a bare pointer.
int64_t strictSearchPlain(const ArrayHeader* arr, uint32_t from, uint32_t end, Value needle) {
    end = std::min(end, arr->length);  // a fromIndex valueOf may have shrunk it
    if (needle.isNumber()) {
        const double n = needle.asNumber();
        if (std::isnan(n)) return -1;
        const Value* elems = arr->elementsData();
        for (uint32_t i = from; i < end; ++i) {
            if (elems[i].isNumber() && elems[i].asNumber() == n) return i;
        }
        return -1;
    }
    const Value* elems = arr->elementsData();
    for (uint32_t i = from; i < end; ++i) {
        if (elems[i].rawBits() == needle.rawBits()) return i;
    }
    return -1;
}

int64_t strictSearchBlock(Rooted<Value>& self, uint32_t from, uint32_t end, Value needle) {
    if (!needle.isString() && !needle.isBigInt()) {
        return strictSearchPlain(self.get().asObject<ArrayHeader>(), from, end, needle);
    }
    const uint16_t tag = needle.tag();
    Rooted<Value> rootedNeedle{needle};
    for (uint32_t i = from; i < end; ++i) {
        // Re-read through the roots: a String compare may flatten a rope.
        const ArrayHeader* arr = self.get().asObject<ArrayHeader>();
        if (i >= arr->length) return -1;
        const Value elem = arr->elementsData()[i];
        if (elem.tag() == tag && bronze_strict_eq(elem.rawBits(), rootedNeedle.get().rawBits())) return i;
    }
    return -1;
}

}  // namespace

uint64_t arrayIndexOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    // `array.indexOf(x)` with no fromIndex, x no String or BigInt: nothing
    // converts or allocates, so the walk needs no roots (what the steps below
    // compute for this case, without their bookkeeping).
    if (argc < 2 && isArray(Value(thisBits))) {
        const Value needle = argc > 0 ? Value(argv[0]) : Value::fromUndefined();
        if (!needle.isString() && !needle.isBigInt() && !rtArrayChainHoldsIndex(Value(thisBits))) {
            const ArrayHeader* arr = Value(thisBits).asObject<ArrayHeader>();
            return Value::fromDouble(static_cast<double>(strictSearchPlain(arr, 0, arr->length, needle))).rawBits();
        }
    }
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "indexOf")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? self.get().asObject<ArrayHeader>()->length
                                             : rtArrayLikeLength(self);
    // 23.1.3.17 step 3: an empty object answers before fromIndex is converted.
    if (len == 0) return Value::fromDouble(-1.0).rawBits();
    uint32_t from = args.count() > 1 ? relativeIndex(toInteger(rtToNumber(args[1])), len) : 0;
    const Value needle = args[0];
    // The block walk answers a hole as absent, which is the chain's answer too
    // unless the chain holds an index; the generic walk reads through it.
    if (isArray(self.get()) && !rtArrayChainHoldsIndex(self.get())) {
        const int64_t found = strictSearchBlock(self, from, len, needle);
        return Value::fromDouble(static_cast<double>(found)).rawBits();
    } else {
        for (uint32_t i = from; i < len; ++i) {
            if (!rtArrayLikeHasElement(self, i)) continue;
            Value elem = rtArrayLikeGetElement(self, i);
            if (bronze_strict_eq(elem.rawBits(), needle.rawBits())) {
                return Value::fromDouble(i).rawBits();
            }
        }
    }
    return Value::fromDouble(-1.0).rawBits();
}

}  // namespace bronze::runtime

// The fast entries (native_fast.h): the receiver an Array with no box, one
// argument. What arrayIndexOf's own first test answers, without the call
// through argv: a needle no String or BigInt compares without a call, and a
// chain with no index lets the block's holes read as absent.
extern "C" uint64_t bronze_fast_array_index_of(uint64_t thisBits, uint64_t needleBits) {
    using namespace bronze;
    using namespace bronze::runtime;
    const Value self(thisBits);
    const Value needle(needleBits);
    if (isArray(self) && !needle.isString() && !needle.isBigInt() && !rtArrayChainHoldsIndex(self)) {
        const ArrayHeader* arr = self.asObject<ArrayHeader>();
        return Value::fromDouble(static_cast<double>(strictSearchPlain(arr, 0, arr->length, needle))).rawBits();
    }
    const uint64_t argv[1] = {needleBits};
    return arrayIndexOf(0, thisBits, 1, argv);
}

namespace bronze::runtime {

// 23.1.3.16 over an Array's block from `from`, while the chain holds no
// index: SameValueZero against every index, a hole read as `undefined`.
// SameValueZero neither converts nor allocates, so the walk needs no roots.
static bool includesBlock(const ArrayHeader* arr, uint32_t from, uint32_t len, Value needle) {
    for (uint32_t i = from; i < len; ++i) {
        if (sameValueZero(arr->getElem(i), needle)) return true;
    }
    return false;
}

}  // namespace bronze::runtime

// The fast entry for `array.includes(x)`: arrayIncludes' own block walk,
// without the call through argv.
extern "C" uint64_t bronze_fast_array_includes(uint64_t thisBits, uint64_t needleBits) {
    using namespace bronze;
    using namespace bronze::runtime;
    const Value self(thisBits);
    if (isArray(self) && !rtArrayChainHoldsIndex(self)) {
        const ArrayHeader* arr = self.asObject<ArrayHeader>();
        return Value::fromBool(includesBlock(arr, 0, arr->length, Value(needleBits))).rawBits();
    }
    const uint64_t argv[1] = {needleBits};
    return arrayIncludes(0, thisBits, 1, argv);
}

namespace bronze::runtime {

uint64_t arrayLastIndexOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "lastIndexOf")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? self.get().asObject<ArrayHeader>()->length
                                             : rtArrayLikeLength(self);
    if (len == 0) return Value::fromDouble(-1.0).rawBits();

    double fromNum = args.count() > 1 && !args[1].isUndefined()
                         ? toInteger(rtToNumber(args[1]))
                         : static_cast<double>(len - 1);
    double k = fromNum >= 0 ? std::min(fromNum, static_cast<double>(len - 1))
                            : static_cast<double>(len) + fromNum;
    if (k < 0) return Value::fromDouble(-1.0).rawBits();

    Value needle = args[0];
    if (isArray(self.get()) && !rtArrayChainHoldsIndex(self.get())) {
        ArrayHeader* arr = self.get().asObject<ArrayHeader>();
        for (int64_t i = static_cast<int64_t>(k); i >= 0; --i) {
            if (!arr->hasElem(static_cast<uint32_t>(i))) continue;
            if (bronze_strict_eq(arr->getElem(static_cast<uint32_t>(i)).rawBits(), needle.rawBits())) {
                return Value::fromDouble(static_cast<double>(i)).rawBits();
            }
        }
    } else {
        for (int64_t i = static_cast<int64_t>(k); i >= 0; --i) {
            uint32_t uidx = static_cast<uint32_t>(i);
            if (!rtArrayLikeHasElement(self, uidx)) continue;
            Value elem = rtArrayLikeGetElement(self, uidx);
            if (bronze_strict_eq(elem.rawBits(), needle.rawBits())) {
                return Value::fromDouble(static_cast<double>(i)).rawBits();
            }
        }
    }
    return Value::fromDouble(-1.0).rawBits();
}

uint64_t arrayIncludes(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "includes")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? self.get().asObject<ArrayHeader>()->length
                                             : rtArrayLikeLength(self);
    // 23.1.3.16 step 3: an empty object answers before fromIndex is converted.
    if (len == 0) return Value::fromBool(false).rawBits();
    uint32_t from = args.count() > 1 ? relativeIndex(toInteger(rtToNumber(args[1])), len) : 0;
    // 23.1.3.16 reads every index with Get, so a hole is `undefined` only when
    // the chain has nothing for it.
    if (isArray(self.get()) && !rtArrayChainHoldsIndex(self.get())) {
        return Value::fromBool(includesBlock(self.get().asObject<ArrayHeader>(), from, len, args[0])).rawBits();
    } else {
        for (uint32_t i = from; i < len; ++i) {
            Value elem = rtArrayLikeGetElement(self, i);
            if (sameValueZero(elem, args[0])) return Value::fromBool(true).rawBits();
        }
    }
    return Value::fromBool(false).rawBits();
}

uint64_t arrayAt(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "at")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? self.get().asObject<ArrayHeader>()->length
                                             : rtArrayLikeLength(self);
    double rel = toInteger(rtToNumber(args.at(0, Value::fromDouble(0.0))));
    double idx = rel < 0 ? static_cast<double>(len) + rel : rel;
    if (idx < 0 || idx >= static_cast<double>(len)) {
        return Value::fromUndefined().rawBits();
    }
    return rtArrayLikeGetElement(self, static_cast<uint32_t>(idx)).rawBits();
}


}  // namespace bronze::runtime
