#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {
namespace typed_array_internal {

// The three questions BOTH buffer surfaces ask -- the plain one in
// builtin_typed_array.cpp and the shared one in builtin_shared_memory.cpp -- so
// they live here rather than being answered twice with two opinions about what
// fits in the heap.
inline bool isBuffer(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == ArrayBufferHeader::kFlags;
}

inline bool toIndex(Value v, const char* what, uint32_t bytesPerElement, uint32_t& out) {
    if (v.isUndefined()) {
        out = 0;
        return true;
    }
    const double n = rtToNumber(v);
    double integer = std::isnan(n) ? 0.0 : std::trunc(n);
    if (integer == 0.0) integer = 0.0;
    if (integer < 0.0 || integer > 9007199254740991.0) {
        rtThrowRangeError(std::string("Invalid ") + what + " length");
        return false;
    }
    if (integer > static_cast<double>(kMaxByteLength) / bytesPerElement) {
        rtThrowRangeError(std::string(what) + " allocation failed: length is too large");
        return false;
    }
    out = static_cast<uint32_t>(integer);
    return true;
}

inline bool checkAllocatable(uint32_t byteLength) {
    const size_t semispace = rtHeap().reserved_size() / 2;
    if (byteLength >= kMaxByteLength || byteLength + 64 >= semispace) {
        rtThrowRangeError("Array buffer allocation failed: " + std::to_string(byteLength) +
                          " bytes does not fit in the heap");
        return false;
    }
    return true;
}

inline bool isTypedArray(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == TypedArrayHeader::kFlags;
}

inline bool isArray(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Array;
}

inline bool requireTypedArray(Value v, const char* method) {
    if (!isTypedArray(v)) {
        rtThrowTypeError(std::string("%TypedArray%.prototype.") + method +
                         " called on a value that is not a typed array");
        return false;
    }
    auto* view = v.asObject<TypedArrayHeader>();
    if (view->buffer.isObject() &&
        view->buffer.asObject<ArrayBufferHeader>()->isDetached()) {
        rtThrowTypeError("ArrayBuffer is detached");
        return false;
    }
    // 23.2.3's ValidateTypedArray ends in IsTypedArrayOutOfBounds, so a view a
    // shrinking `resize` stranded — closed but not detached — is a TypeError
    // from every prototype method, not an empty array. Element access is the
    // one surface that answers such a view softly (undefined reads, discarded
    // writes); the methods all throw.
    if (view->isOutOfBounds()) {
        rtThrowTypeError("TypedArray is out of bounds of its ArrayBuffer");
        return false;
    }
    return true;
}

inline uint32_t lengthOf(Value v) { return v.asObject<TypedArrayHeader>()->length; }
inline double elemOf(Value v, uint32_t i) { return v.asObject<TypedArrayHeader>()->get(i); }
inline ElementKind kindOf(Value v) { return v.asObject<TypedArrayHeader>()->elementKind(); }

inline double toInteger(double d) {
    if (std::isnan(d)) return 0.0;
    if (std::isinf(d)) return d;
    const double t = std::trunc(d);
    return t == 0.0 ? 0.0 : t;
}

inline uint32_t relativeIndex(double rel, uint32_t len) {
    if (rel < 0) {
        const double from = static_cast<double>(len) + rel;
        return from < 0 ? 0u : static_cast<uint32_t>(from);
    }
    return static_cast<uint32_t>(std::min(rel, static_cast<double>(len)));
}

inline void relativeArg(Value v, uint32_t len, uint32_t& out, uint32_t fallback) {
    out = v.isUndefined() ? fallback : relativeIndex(toInteger(rtToNumber(v)), len);
}

inline Value newViewLike(Value model, uint32_t length) {
    return Value::fromObject(TypedArrayHeader::create(rtHeap(), kindOf(model), length));
}

inline bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

inline bool requireCallable(Value v, const char* method) {
    if (isCallable(v)) return true;
    rtThrowTypeError(std::string("%TypedArray%.prototype.") + method + " callback is not a function");
    return false;
}

inline Value callBack(Rooted<Value>& fn, Rooted<Value>& thisArg, Rooted<Value>& elem, uint32_t index,
                      Rooted<Value>& self) {
    Value block[3] = {elem.get(), Value::fromDouble(static_cast<double>(index)), self.get()};
    return Value(bronze_dynamic_call(fn.get().rawBits(), thisArg.get().rawBits(), 3,
                                     reinterpret_cast<const uint64_t*>(block)));
}

inline bool sameValueZero(Value a, Value b) {
    if (a.isNumber() && b.isNumber()) {
        const double da = a.asNumber();
        const double db = b.asNumber();
        if (std::isnan(da) && std::isnan(db)) return true;
        return da == db;
    }
    return bronze_strict_eq(a.rawBits(), b.rawBits());
}

}  // namespace typed_array_internal

using namespace typed_array_internal;

// Transform & Mutator declarations:
uint64_t taSet(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taSubarray(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taSlice(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taFill(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taCopyWithin(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taReverse(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taSort(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taJoin(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taToReversed(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taToSorted(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taWith(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taAt(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

// Search & Iteration declarations:
uint64_t taIndexOf(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taLastIndexOf(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taIncludes(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taFind(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taFindIndex(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taFindLast(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taFindLastIndex(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taForEach(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taMap(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taFilter(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taEvery(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taSome(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taReduce(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taReduceRight(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taKeys(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taValues(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t taEntries(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

}  // namespace bronze::runtime
