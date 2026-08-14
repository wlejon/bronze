#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/integrity.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

inline bool isArray(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Array;
}

inline bool requireArray(Value v, const char* method) {
    if (isArray(v)) return true;
    rtThrowTypeError(std::string("Array.prototype.") + method +
                     " called on a value that is not an array");
    return false;
}

inline uint32_t lengthOf(Value v) { return v.asObject<ArrayHeader>()->length; }
inline Value elemOf(Value v, uint32_t i) { return v.asObject<ArrayHeader>()->getElem(i); }
inline bool hasIndex(Value self, uint32_t i) { return self.asObject<ArrayHeader>()->hasElem(i); }

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

inline bool sameValueZero(Value a, Value b) {
    if (a.isNumber() && b.isNumber()) {
        const double da = a.asNumber();
        const double db = b.asNumber();
        if (std::isnan(da) && std::isnan(db)) return true;
        return da == db;
    }
    return bronze_strict_eq(a.rawBits(), b.rawBits());
}

inline Value newArray() { return Value(bronze_create_array(0)); }

inline void appendTo(Rooted<Value>& arrRoot, Rooted<Value>& val) {
    bronze_array_append(arrRoot.get().rawBits(), val.get().rawBits());
}

inline void appendHole(Rooted<Value>& out) {
    const uint32_t at = lengthOf(out.get());
    ArrayHeader::setLength(rtHeap(), out, at + 1);
    out.get().asObject<ArrayHeader>()->deleteElem(at);
}

inline bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

inline bool requireCallable(Value v, const char* method) {
    if (isCallable(v)) return true;
    rtThrowTypeError(std::string("Array.prototype.") + method + " callback is not a function");
    return false;
}

inline Value callBack(Rooted<Value>& fn, Rooted<Value>& thisArg, Rooted<Value>& elem, uint32_t index,
                      Rooted<Value>& self) {
    Value block[3] = {elem.get(), Value::fromDouble(static_cast<double>(index)), self.get()};
    return Value(bronze_dynamic_call(fn.get().rawBits(), thisArg.get().rawBits(), 3,
                                     reinterpret_cast<const uint64_t*>(block)));
}

inline bool requireExtensible(Value self, const char* method) {
    if (rtIsExtensible(self)) return true;
    rtThrowTypeError(std::string("Cannot add elements to an array that is not extensible (Array.prototype.") +
                     method + ")");
    return false;
}

inline bool requireConfigurableElements(Value self, const char* method) {
    if (rtArrayElementsConfigurable(self)) return true;
    rtThrowTypeError(std::string("Cannot modify elements of a sealed array (Array.prototype.") +
                     method + ")");
    return false;
}

inline bool requireWritableElements(Value self, const char* method) {
    if (rtIntegrityLevel(self) != IntegrityLevel::Frozen) return true;
    rtThrowTypeError(std::string("Cannot assign to read only element of a frozen array (Array.prototype.") +
                     method + ")");
    return false;
}

// Species & Spreadable helpers
Value rtArraySpeciesCreate(Rooted<Value>& originalArray, uint32_t length);
bool rtIsConcatSpreadable(Rooted<Value>& item);

// Declarations of method entry points:
// Mutators
uint64_t arrayPush(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayPop(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayShift(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayUnshift(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayReverse(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayFill(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arraySplice(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayCopyWithin(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

// Search
uint64_t arrayIndexOf(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayLastIndexOf(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayIncludes(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayAt(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayFind(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayFindIndex(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayFindLast(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayFindLastIndex(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

// Transform
uint64_t arraySlice(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayConcat(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayJoin(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayFlat(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayFlatMap(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayToSorted(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayToReversed(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayToSpliced(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayWith(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

// Iteration
uint64_t arrayForEach(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayMap(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayFilter(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arraySome(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayEvery(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayReduce(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t arrayReduceRight(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

}  // namespace bronze::runtime
