// An array's own properties that are NOT elements: `length`, and the named
// ones.
//
// The seam is a receiver kind's non-element storage, and it exists because six
// paths have to agree about it — a read, a write, `in`, `delete`, `for-in` and
// the own-key walks — and each of them reaches an array from its own file. The
// question "does this array have an own property called `k`" was answered in
// three of those places by hand before there was anything but a match array's
// `index` to answer for; a program can write one now, so the answer moved here
// and each path asks rather than restates it.
//
// WHERE A NAMED PROPERTY LIVES. In `ArrayHeader::properties`, the side object
// that already held a match array's `index`/`input`/`groups` and an `arguments`
// object's `callee` — a plain object with a NULL prototype, so it is storage
// and not a link in the array's chain (array.cpp says why that matters). Two
// consequences are the whole design:
//
//   - the ELEMENT path is untouched. An array that never takes a named write
//     has no side object, the same header it always had, and the element fast
//     path reaches the block without loading anything new.
//   - the collector needs nothing: `properties` is a Value in the array's
//     payload, which the generic payload scan already forwards.
//
// `length` is not a named property and never lands in that object. It is the
// exotic own property of 10.4.2, stored as the array's `length` field, and
// writing it is ArraySetLength (10.4.2.4) — a truncation or a run of holes,
// which is why it is here rather than beside the ordinary write.

#include <cmath>
#include <string>

#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/integrity.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

bool rtArrayOwnNamed(Value arrVal, PropertyKey name, PropertyInfo& out) {
    const Value props = arrVal.asObject<ArrayHeader>()->properties;
    if (!props.isObject()) return false;
    ObjectHeader* holder = props.asObject<ObjectHeader>();
    return holder->shape && holder->shape->lookupProperty(name, out);
}

std::vector<StringHeader*> rtArrayOwnNamedKeys(Value arrVal, bool enumerableOnly) {
    const Value props = arrVal.asObject<ArrayHeader>()->properties;
    if (!props.isObject()) return {};
    return rtOwnStringKeysOrdered(props.asObject<ObjectHeader>(), enumerableOnly);
}

SetRefusal rtArrayNamedSet(Rooted<Value>& arr, Rooted<Value>& key, Rooted<Value>& val) {
    ArrayHeader::ensureProperties(rtHeap(), rtArena(), arr);
    Rooted<Value> props{arr.get().asObject<ArrayHeader>()->properties};
    SetRefusal refusal = SetRefusal::None;
    // The RECEIVER is the array, not the box its named properties live in: an
    // accessor reached through this write must see the array as `this`, or a
    // setter would be handed the storage rather than the object the program
    // wrote to. The box's own [[Extensible]] answers for the array's, which is
    // where `Object.preventExtensions(a); a.foo = 1` is refused — the level is
    // recorded in that object's dictionary (integrity.h).
    //
    // No inline cache. An entry describes a shape, and the shape here belongs
    // to the side object while the site's receiver is the array; generated code
    // never reaches this path with a cached hit (it checks for a plain object
    // first), and filling the entry would leave a shape behind that only a
    // different receiver kind could match.
    props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, /*ic=*/nullptr,
                                                  /*enumerable=*/true, /*defineOwn=*/false,
                                                  arr.slot_ptr(), &refusal);
    return refusal;
}

bool rtArrayNamedDelete(Value arrVal, PropertyKey name) {
    const Value props = arrVal.asObject<ArrayHeader>()->properties;
    // Nothing was ever written, so the property is already absent — which is
    // the state `delete` wants, and 13.5.1 answers true for it.
    if (!props.isObject()) return true;
    return props.asObject<ObjectHeader>()->deleteProperty(rtArena(), name);
}

SetRefusal rtArraySetLength(Rooted<Value>& arr, Value newLenVal) {
    // 10.4.2.4 steps 3 and 4: ToUint32 and ToNumber of the value must have the
    // same mathematical value, or it was never an array length. NaN, a
    // negative, a fraction and 2^32 all fail this and all get the RangeError
    // the specification names — the same one `new Array(x)` raises, because it
    // is the same test.
    const double num = rtToNumber(newLenVal);
    if (!(num >= 0.0) || num > 4294967295.0 || std::floor(num) != num) {
        rtThrowRangeError("Invalid array length");
        return SetRefusal::None;
    }
    const auto newLen = static_cast<uint32_t>(num);
    const uint32_t oldLen = arr.get().asObject<ArrayHeader>()->length;
    // 10.1.6.3 step 4: a write of the SAME value to a non-writable property
    // succeeds, so the frozen check below asks about a change rather than about
    // the write.
    if (newLen == oldLen) return SetRefusal::None;
    // `Object.freeze` is what makes `length` non-writable (7.3.14 stamps
    // `writable: false` on every own property, and `length` is one).
    if (rtIntegrityLevel(arr.get()) == IntegrityLevel::Frozen) {
        return SetRefusal::NotWritable;
    }

    if (newLen > oldLen) {
        // The new slots are HOLES and not `undefined`: 10.4.2.4 only moves
        // `length`, and nothing it moves past becomes an own property. bronze's
        // elements are dense, so the holes are stored — and a length the
        // language allows is therefore not thereby a length this heap can hold.
        // Refused BEFORE the allocation, so `std::bad_alloc` never unwinds out
        // of a helper generated code called (the rule `new Array(n)` follows).
        const size_t bytes = static_cast<size_t>(newLen) * sizeof(Value);
        if (bytes + 64 >= rtHeap().reserved_size() / 2) {
            rtThrowRangeError("Array allocation failed: " + std::to_string(newLen) +
                              " elements does not fit in the heap");
            return SetRefusal::None;
        }
        ArrayHeader::setLength(rtHeap(), arr, newLen);
        return SetRefusal::None;
    }

    // Shrinking DELETES the elements above the new length (10.4.2.4 step 17),
    // so a SEALED array — whose elements 7.3.14 made non-configurable — stops
    // at the highest one it cannot remove and reports false. A hole is not an
    // own property and is passed over freely, which is why the stop is the
    // highest PRESENT index rather than simply `oldLen`.
    uint32_t stop = newLen;
    if (!rtArrayElementsConfigurable(arr.get())) {
        const ArrayHeader* a = arr.get().asObject<ArrayHeader>();
        for (uint32_t i = oldLen; i > newLen; --i) {
            if (a->hasElem(i - 1)) {
                stop = i;
                break;
            }
        }
    }
    ArrayHeader::setLength(rtHeap(), arr, stop);
    return stop == newLen ? SetRefusal::None : SetRefusal::NotWritable;
}

}  // namespace bronze::runtime
