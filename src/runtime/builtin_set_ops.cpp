// The ES2025 SET OPERATIONS (ECMA-262 24.2.4): `union`, `intersection`,
// `difference`, `symmetricDifference`, `isSubsetOf`, `isSupersetOf` and
// `isDisjointFrom`.
//
// Their own translation unit, and not more rows in builtin_map.cpp, because they
// are the only members of a Set that read a SECOND collection — and they do it
// through a protocol rather than by touching another Set's table. 24.2.1.2
// GetSetRecord reads `size`, `has` and `keys` off the argument and uses nothing
// else, so every one of these works on a Map (whose `keys` yields its keys), on
// a hand-written `{size, has, keys}`, and on a subclass that overrides `has` —
// and none of them works on an Array, which has no `size`. That is the whole
// design of the proposal and it is why the argument is never brand-checked.
//
// Two rules from 24.2.4 that a reader will look for:
//
//   - the ORDER of the result is this set's order. Every member below either
//     starts from a copy of this set's entries or filters them in place, and the
//     one member whose algorithm collects from the other side (`intersection`,
//     on its large-other arm) re-orders afterwards for exactly that reason.
//   - which side gets WALKED is chosen by the two sizes, and it is observable:
//     the small side is iterated and the large side is asked `has`. A program
//     with a counting `has` can see the difference, so the branch is the spec's
//     and not an optimisation this file is free to drop.

#include <cmath>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/iterator.h"
#include "runtime/iterator_helpers_internal.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

using iterator_helpers::closeIterator;
using iterator_helpers::isCallable;
using iterator_helpers::Step;
using iterator_helpers::stepIterator;

bool isSetObject(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == MapHeader::kSetFlags;
}

// 24.2.4's step 2 in every member: `this` must really be a Set. The argument is
// deliberately NOT checked this way — see the file header.
bool requireSet(Value self, const char* member) {
    if (isSetObject(self)) return true;
    rtThrowTypeError("Set.prototype." + std::string(member) +
                     " called on a value that is not a Set");
    return false;
}

// 7.2.11 CanonicalizeKeyedCollectionKey: -0 is stored as +0, so that a set
// built from another collection's keys cannot end up holding a key that
// `Set.prototype.has(0)` would find but `[...set][0]` would print as -0.
Value canonicalizeKey(Value v) {
    if (v.isNumber() && v.asNumber() == 0.0) return Value::fromDouble(0.0);
    return v;
}

// A Set Record (24.2.1.2 GetSetRecord): the object, its claimed size, and its
// `has` and `keys` methods. The size is read and validated BEFORE either method,
// which is the order the errors come out in — `{}` is "size is NaN", not "has is
// not a function".
struct SetRecord {
    Rooted<Value> object;
    Rooted<Value> has;
    Rooted<Value> keys;
    double size = 0.0;
};

Value genericGet(Rooted<Value>& obj, const char* key) {
    Rooted<Value> keyRoot{rtMakeString(key)};
    return Value(bronze_elem_get(obj.get().rawBits(), keyRoot.get().rawBits()));
}

bool getSetRecord(Rooted<Value>& other, const char* member, SetRecord& out) {
    const std::string where = "Set.prototype." + std::string(member);
    if (!other.get().isObject()) {
        rtThrowTypeError(where + " requires a set-like object, not " +
                         rtIterableKindName(other.get()));
        return false;
    }
    out.object.set(other.get());
    Rooted<Value> rawSize{genericGet(other, "size")};
    if (rtExceptionPending()) return false;
    const double numSize = rtToNumber(rawSize.get());
    if (rtExceptionPending()) return false;
    // Step 4: NaN — which is what an absent `size` becomes — is a TypeError and
    // not a zero-sized set. An Array reaches exactly here, because `length` is
    // not `size`.
    if (std::isnan(numSize)) {
        rtThrowTypeError(where + " requires a set-like object with a numeric `size`");
        return false;
    }
    if (numSize < 0.0) {
        rtThrowRangeError(where + " requires a set-like object with a non-negative `size`");
        return false;
    }
    out.size = std::isinf(numSize) ? numSize : std::trunc(numSize);
    out.has.set(genericGet(other, "has"));
    if (rtExceptionPending()) return false;
    if (!isCallable(out.has.get())) {
        rtThrowTypeError(where + " requires a set-like object with a `has` method");
        return false;
    }
    out.keys.set(genericGet(other, "keys"));
    if (rtExceptionPending()) return false;
    if (!isCallable(out.keys.get())) {
        rtThrowTypeError(where + " requires a set-like object with a `keys` method");
        return false;
    }
    return true;
}

// 7.4.3 GetIteratorFromMethod over the record's `keys`: call it on the object,
// then read the `next` of whatever it returned — once, as the protocol requires.
bool openKeys(SetRecord& rec, Rooted<Value>& iterOut, Rooted<Value>& nextOut) {
    iterOut.set(rec.keys.get().asObject<FunctionHeader>()->call(rec.object.get(), 0, nullptr));
    if (rtExceptionPending()) return false;
    if (!iterOut.get().isObject()) {
        rtThrowTypeError("the `keys` method of a set-like object did not return an object");
        return false;
    }
    nextOut.set(genericGet(iterOut, "next"));
    return !rtExceptionPending();
}

// `Call(rec.[[Has]], rec.[[SetObject]], «value»)`, as truthiness. The receiver
// is the RECORD's object and not the value, which is what lets a Map stand in
// for a set-like: `Map.prototype.has` needs its own map as `this`.
bool otherHas(SetRecord& rec, Rooted<Value>& value, bool& out) {
    Value block[1] = {value.get()};
    Rooted<Value> result{rec.has.get().asObject<FunctionHeader>()->call(rec.object.get(), 1, block)};
    if (rtExceptionPending()) return false;
    out = bronze_truthy(result.get().rawBits());
    return true;
}

// ---- the result set ---------------------------------------------------------

// A fresh empty Set. It carries no prototype link, exactly as `new Set()` does
// here (builtin_map.cpp's header says why a Set has no prototype object in
// bronze), so a returned set is indistinguishable from a constructed one.
Value newSet() { return Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kSetFlags)); }

// Append unless already present. `MapHeader::set` is the one insertion path and
// it already keeps an existing key's POSITION, so this is the whole of "if
// resultSetData does not contain value, append value".
void addTo(Rooted<Value>& set, Rooted<Value>& value) {
    Rooted<Value> key{canonicalizeKey(value.get())};
    MapHeader::set(rtHeap(), set, key, key);
}

bool setHas(Rooted<Value>& set, Rooted<Value>& value) {
    Rooted<Value> key{canonicalizeKey(value.get())};
    return MapHeader::find(rtHeap(), set, key) != UINT32_MAX;
}

// This set's live elements, as a snapshot. It is a snapshot because every loop
// below calls USER CODE per element — `has` on the other side, or a `keys`
// iterator — and user code can add to or clear the set being walked. 24.2.4
// reads `O.[[SetData]]` as it stood when the member was entered, and a cursor
// into the live table would instead see the mutation.
//
// The values are held in a RootedBlock rather than a plain vector: the walk
// allocates, so a std::vector<Value> of raw bits would be a block of stale
// addresses after the first collection.
void fillLiveElements(Rooted<Value>& set, RootedBlock& block) {
    auto* map = set.get().asObject<MapHeader>();
    const uint32_t used = map->used();
    uint32_t at = 0;
    for (uint32_t slot = 0; slot < used; ++slot) {
        if (!map->liveAt(slot)) continue;
        block.set(at++, map->keyAt(slot));
        // Re-derived per element: nothing here allocates, but the pointer is
        // taken from the root each turn so that a later edit cannot make this
        // loop the one that holds a stale header.
        map = set.get().asObject<MapHeader>();
    }
}

// The count `liveElements` will fill, and the size every member compares against
// the other side's.
uint32_t liveCount(Rooted<Value>& set) {
    return set.get().asObject<MapHeader>()->liveSize();
}

// ---- 24.2.4.17 union --------------------------------------------------------

uint64_t setUnion(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireSet(self.get(), "union")) return Value::fromUndefined().rawBits();
    Rooted<Value> other{args[0]};
    SetRecord rec;
    if (!getSetRecord(other, "union", rec)) return Value::fromUndefined().rawBits();
    Rooted<Value> iter;
    Rooted<Value> next;
    if (!openKeys(rec, iter, next)) return Value::fromUndefined().rawBits();

    // Step 4: the result STARTS as a copy of this set, so this set's elements
    // keep their order and come first.
    Rooted<Value> result{newSet()};
    {
        RootedBlock block(liveCount(self));
        fillLiveElements(self, block);
        for (uint32_t i = 0; i < block.count(); ++i) {
            Rooted<Value> element{Value(block.data()[i])};
            addTo(result, element);
        }
    }
    for (;;) {
        Rooted<Value> value;
        const Step step = stepIterator(iter, next, value);
        if (step == Step::Threw) return Value::fromUndefined().rawBits();
        if (step == Step::Done) break;
        addTo(result, value);
    }
    return result.get().rawBits();
}

// ---- 24.2.4.9 intersection --------------------------------------------------

uint64_t setIntersection(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireSet(self.get(), "intersection")) return Value::fromUndefined().rawBits();
    Rooted<Value> other{args[0]};
    SetRecord rec;
    if (!getSetRecord(other, "intersection", rec)) return Value::fromUndefined().rawBits();

    Rooted<Value> result{newSet()};
    const uint32_t thisSize = liveCount(self);
    if (static_cast<double>(thisSize) <= rec.size) {
        // Step 6: the smaller side is walked and the larger is asked. Walking
        // this set also gives the result its order for free.
        RootedBlock block(thisSize);
        fillLiveElements(self, block);
        for (uint32_t i = 0; i < block.count(); ++i) {
            Rooted<Value> element{Value(block.data()[i])};
            bool present = false;
            if (!otherHas(rec, element, present)) return Value::fromUndefined().rawBits();
            if (present) addTo(result, element);
        }
        return result.get().rawBits();
    }
    // Step 7: the other side is walked. Its keys arrive in ITS order, so the
    // matches are collected first and then re-emitted in this set's order —
    // step 7.b.iv's "sort resultSetData into the same relative order as
    // O.[[SetData]]", performed as a second pass rather than as a sort.
    Rooted<Value> iter;
    Rooted<Value> next;
    if (!openKeys(rec, iter, next)) return Value::fromUndefined().rawBits();
    Rooted<Value> matched{newSet()};
    for (;;) {
        Rooted<Value> value;
        const Step step = stepIterator(iter, next, value);
        if (step == Step::Threw) return Value::fromUndefined().rawBits();
        if (step == Step::Done) break;
        if (setHas(self, value)) addTo(matched, value);
    }
    RootedBlock block(thisSize);
    fillLiveElements(self, block);
    for (uint32_t i = 0; i < block.count(); ++i) {
        Rooted<Value> element{Value(block.data()[i])};
        if (setHas(matched, element)) addTo(result, element);
    }
    return result.get().rawBits();
}

// ---- 24.2.4.5 difference ----------------------------------------------------

uint64_t setDifference(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireSet(self.get(), "difference")) return Value::fromUndefined().rawBits();
    Rooted<Value> other{args[0]};
    SetRecord rec;
    if (!getSetRecord(other, "difference", rec)) return Value::fromUndefined().rawBits();

    // The result starts as a copy of this set and elements are REMOVED from it,
    // which is what keeps this set's order however the other side is walked.
    Rooted<Value> result{newSet()};
    const uint32_t thisSize = liveCount(self);
    {
        RootedBlock block(thisSize);
        fillLiveElements(self, block);
        for (uint32_t i = 0; i < block.count(); ++i) {
            Rooted<Value> element{Value(block.data()[i])};
            addTo(result, element);
        }
    }
    if (static_cast<double>(thisSize) <= rec.size) {
        RootedBlock block(thisSize);
        fillLiveElements(self, block);
        for (uint32_t i = 0; i < block.count(); ++i) {
            Rooted<Value> element{Value(block.data()[i])};
            bool present = false;
            if (!otherHas(rec, element, present)) return Value::fromUndefined().rawBits();
            if (present) {
                Rooted<Value> key{canonicalizeKey(element.get())};
                MapHeader::remove(rtHeap(), result, key);
            }
        }
        return result.get().rawBits();
    }
    Rooted<Value> iter;
    Rooted<Value> next;
    if (!openKeys(rec, iter, next)) return Value::fromUndefined().rawBits();
    for (;;) {
        Rooted<Value> value;
        const Step step = stepIterator(iter, next, value);
        if (step == Step::Threw) return Value::fromUndefined().rawBits();
        if (step == Step::Done) break;
        Rooted<Value> key{canonicalizeKey(value.get())};
        MapHeader::remove(rtHeap(), result, key);
    }
    return result.get().rawBits();
}

// ---- 24.2.4.14 symmetricDifference ------------------------------------------

uint64_t setSymmetricDifference(uint64_t, uint64_t thisBits, uint32_t argc,
                                const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireSet(self.get(), "symmetricDifference")) return Value::fromUndefined().rawBits();
    Rooted<Value> other{args[0]};
    SetRecord rec;
    if (!getSetRecord(other, "symmetricDifference", rec)) {
        return Value::fromUndefined().rawBits();
    }
    // This member always walks the other side — there is no size branch,
    // because every element of both sides has to be looked at.
    Rooted<Value> iter;
    Rooted<Value> next;
    if (!openKeys(rec, iter, next)) return Value::fromUndefined().rawBits();

    Rooted<Value> result{newSet()};
    const uint32_t thisSize = liveCount(self);
    {
        RootedBlock block(thisSize);
        fillLiveElements(self, block);
        for (uint32_t i = 0; i < block.count(); ++i) {
            Rooted<Value> element{Value(block.data()[i])};
            addTo(result, element);
        }
    }
    for (;;) {
        Rooted<Value> value;
        const Step step = stepIterator(iter, next, value);
        if (step == Step::Threw) return Value::fromUndefined().rawBits();
        if (step == Step::Done) break;
        // Step 5.d.i tests THIS SET, not the result being built: a value the
        // other side yields twice must not be removed and then re-added, and
        // that is exactly what testing the result would do.
        if (setHas(self, value)) {
            Rooted<Value> key{canonicalizeKey(value.get())};
            MapHeader::remove(rtHeap(), result, key);
        } else {
            addTo(result, value);
        }
    }
    return result.get().rawBits();
}

// ---- the three predicates (24.2.4.11-13) ------------------------------------

uint64_t setIsSubsetOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireSet(self.get(), "isSubsetOf")) return Value::fromUndefined().rawBits();
    Rooted<Value> other{args[0]};
    SetRecord rec;
    if (!getSetRecord(other, "isSubsetOf", rec)) return Value::fromUndefined().rawBits();
    const uint32_t thisSize = liveCount(self);
    // Step 4: a set larger than the other cannot be a subset of it, and the
    // answer costs no calls at all. It rests on `size` being honest, which is
    // the record's contract.
    if (static_cast<double>(thisSize) > rec.size) return Value::fromBool(false).rawBits();
    RootedBlock block(thisSize);
    fillLiveElements(self, block);
    for (uint32_t i = 0; i < block.count(); ++i) {
        Rooted<Value> element{Value(block.data()[i])};
        bool present = false;
        if (!otherHas(rec, element, present)) return Value::fromUndefined().rawBits();
        if (!present) return Value::fromBool(false).rawBits();
    }
    return Value::fromBool(true).rawBits();
}

uint64_t setIsSupersetOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireSet(self.get(), "isSupersetOf")) return Value::fromUndefined().rawBits();
    Rooted<Value> other{args[0]};
    SetRecord rec;
    if (!getSetRecord(other, "isSupersetOf", rec)) return Value::fromUndefined().rawBits();
    if (static_cast<double>(liveCount(self)) < rec.size) return Value::fromBool(false).rawBits();
    Rooted<Value> iter;
    Rooted<Value> next;
    if (!openKeys(rec, iter, next)) return Value::fromUndefined().rawBits();
    for (;;) {
        Rooted<Value> value;
        const Step step = stepIterator(iter, next, value);
        if (step == Step::Threw) return Value::fromUndefined().rawBits();
        if (step == Step::Done) return Value::fromBool(true).rawBits();
        if (setHas(self, value)) continue;
        // Step 6.e: the answer is known, so the other side's iterator is CLOSED
        // rather than abandoned — a generator standing in for `keys` gets its
        // `finally`.
        closeIterator(iter, /*suppress=*/false);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        return Value::fromBool(false).rawBits();
    }
}

uint64_t setIsDisjointFrom(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireSet(self.get(), "isDisjointFrom")) return Value::fromUndefined().rawBits();
    Rooted<Value> other{args[0]};
    SetRecord rec;
    if (!getSetRecord(other, "isDisjointFrom", rec)) return Value::fromUndefined().rawBits();
    const uint32_t thisSize = liveCount(self);
    if (static_cast<double>(thisSize) <= rec.size) {
        RootedBlock block(thisSize);
        fillLiveElements(self, block);
        for (uint32_t i = 0; i < block.count(); ++i) {
            Rooted<Value> element{Value(block.data()[i])};
            bool present = false;
            if (!otherHas(rec, element, present)) return Value::fromUndefined().rawBits();
            if (present) return Value::fromBool(false).rawBits();
        }
        return Value::fromBool(true).rawBits();
    }
    Rooted<Value> iter;
    Rooted<Value> next;
    if (!openKeys(rec, iter, next)) return Value::fromUndefined().rawBits();
    for (;;) {
        Rooted<Value> value;
        const Step step = stepIterator(iter, next, value);
        if (step == Step::Threw) return Value::fromUndefined().rawBits();
        if (step == Step::Done) return Value::fromBool(true).rawBits();
        if (!setHas(self, value)) continue;
        closeIterator(iter, /*suppress=*/false);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        return Value::fromBool(false).rawBits();
    }
}

}  // namespace

const NativeMethod* rtSetOperationMethods(size_t& count) {
    // 24.2.4's seven, each with the spec's `length` of 1.
    static const NativeMethod kMethods[] = {
        {"union", setUnion, 1},
        {"intersection", setIntersection, 1},
        {"difference", setDifference, 1},
        {"symmetricDifference", setSymmetricDifference, 1},
        {"isSubsetOf", setIsSubsetOf, 1},
        {"isSupersetOf", setIsSupersetOf, 1},
        {"isDisjointFrom", setIsDisjointFrom, 1},
    };
    count = sizeof(kMethods) / sizeof(kMethods[0]);
    return kMethods;
}

}  // namespace bronze::runtime
