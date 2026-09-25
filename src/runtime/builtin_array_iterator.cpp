// `Array.prototype.values` / `keys` / `entries` (ECMA-262 23.1.3.34, 23.1.3.17,
// 23.1.3.5) and the ArrayIterator object they hand back (23.1.5).
//
// `for-of` and spread never reach this: `rtOpenIterator` steps an array with a
// cursor and no iterator object at all (iterator.cpp). What this file exists
// for is a program that HOLDS the iterator — `a[Symbol.iterator]`, a manual
// `.next()` loop, `for (const [i, v] of a.entries())` — which must see the
// same values the cursor walk sees. It is builtin_typed_array_methods.cpp's
// iterator arrangement with 23.1.5.1's [[Kind]] slot added, because an array
// exposes all three kinds where a typed array exposes only `values`.
//
// The identity 23.1.3.41 pins — `Array.prototype[Symbol.iterator]` IS
// `Array.prototype.values`, one function object and not a twin — costs nothing
// here: `rtNativeFunction` interns on the code pointer, so every route to
// `rtArrayValuesBuiltin` answers the same object, and `===` holds between
// `a[Symbol.iterator]`, `a.values` and `Array.prototype.values`.

#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isArray(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Array;
}

// 23.1.5.1's three kinds, in the values the [[Kind]] slot stores.
enum IterKind : uint32_t { Keys = 0, Values = 1, Entries = 2 };

// The slot helpers every iterator file carries: neither allocates, and the
// caller re-reads the object out of its root each time regardless, because
// the code around them does allocate.
Value readSlot(Rooted<Value>& obj, uint32_t slot) {
    return obj.get().asObject<ObjectHeader>()->internalSlot(slot);
}

void writeSlot(Rooted<Value>& obj, uint32_t slot, Value val) {
    obj.get().asObject<ObjectHeader>()->setInternalSlot(slot, val);
}

// 23.1.5.2.1 step 15's `[index, element]` pair.
Value makePair(Rooted<Value>& a, Rooted<Value>& b) {
    Rooted<Value> pair{Value(bronze_create_array(2))};
    pair.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, a);
    pair.get().asObject<ArrayHeader>()->setElem(rtHeap(), 1, b);
    return pair.get();
}

uint64_t arrayIterNext(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    // 23.1.5.2.1 step 2: a receiver without the internal slots is a TypeError,
    // and the brand is what makes the slot reads below memory-safe at all.
    if (!rtIsIteratorObject(self.get(), IteratorProto::Array)) {
        return rtThrowTypeError("next called on an incompatible receiver").rawBits();
    }
    Value out = Value::fromUndefined();
    const bool more = rtArrayIteratorStep(self, out);
    Rooted<Value> produced{out};
    return rtCreateIterResult(produced, !more).rawBits();
}

// 23.1.5.1 CreateArrayIterator. The `next` is an own property of the iterator
// rather than a member of %ArrayIteratorPrototype% — the divergence
// cases/collection_internal_slots.js records for every iterator kind — and
// the `[Symbol.iterator]` self-hook is INHERITED from %IteratorPrototype%,
// so the object has no own symbol-keyed property at all.
uint64_t makeArrayIterator(uint64_t thisBits, uint32_t kind, const char* method) {
    Rooted<Value> self{Value(thisBits)};
    // 23.1.3.34 step 1 is ToObject(this value): the method is GENERIC, and
    // 23.1.5.1 reads the receiver through `LengthOfArrayLike` and `Get`, so a
    // Proxy over an array iterates through its `get` trap — `length` on every
    // step, then the element — and an array-like iterates its indices. Only
    // null and undefined are refused (7.1.18); a primitive boxes to an object
    // with no `length`, which the generic walk below reads as 0.
    if (self.get().isNull() || self.get().isUndefined()) {
        return rtThrowTypeError(std::string("Array.prototype.") + method +
                                " called on null or undefined")
            .rawBits();
    }
    Rooted<Value> it{rtNewIteratorObject(IteratorProto::Array)};
    Rooted<Value> nextFn{rtNativeFunction(arrayIterNext, 0, "next", 0)};
    Rooted<Value> nk{rtMakeString("next")};
    it.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), nk, nextFn);
    writeSlot(it, ArrayIteratorSlot::IteratedArrayLike, self.get());
    writeSlot(it, ArrayIteratorSlot::NextIndex, Value::fromDouble(0.0));
    writeSlot(it, ArrayIteratorSlot::Kind, Value::fromDouble(static_cast<double>(kind)));
    return it.get().rawBits();
}

}  // namespace

// 23.1.5.2.1 %ArrayIteratorPrototype%.next steps 4-15, without the result
// object: what `for (const [i, v] of a.entries())` steps through once
// `rtOpenIterator` has seen the object's own `next` is still `arrayIterNext`.
// The brand check is the caller's.
bool rtArrayIteratorStep(Rooted<Value>& self, Value& produced) {
    Rooted<Value> target{readSlot(self, ArrayIteratorSlot::IteratedArrayLike)};
    // The prototype is shared with a typed array's iterator (23.2.5.2), so a
    // detached `next` moved between the two kinds lands here with the other
    // kind's target — exhausted, not crashed, exactly as taIterNext answers
    // an array-backed receiver. A slot holding no object is the same answer.
    if (!target.get().isObject() || target.get().asObject<HeapObjectHeader>()->flags ==
                                        TypedArrayHeader::kFlags) {
        return false;
    }
    const auto at = static_cast<uint32_t>(readSlot(self, ArrayIteratorSlot::NextIndex).asNumber());
    // 23.1.5.1 step b.ii: `LengthOfArrayLike(array)` on EVERY step — an
    // array's own length word, any other object's `length` through Get (a
    // proxy's `get` trap), which is what makes an array grown mid-walk
    // iterate its new tail and a trap see one `length` read per element.
    const uint32_t length = rtArrayLikeLength(target);
    if (rtExceptionPending()) return false;
    if (at >= length) return false;
    const auto kind = static_cast<uint32_t>(readSlot(self, ArrayIteratorSlot::Kind).asNumber());

    writeSlot(self, ArrayIteratorSlot::NextIndex, Value::fromDouble(static_cast<double>(at + 1)));
    if (kind == Keys) {
        produced = Value::fromDouble(static_cast<double>(at));
        return true;
    }
    // 23.1.5.2.1 reads with Get, so a HOLE iterates as `undefined` rather
    // than being skipped — the same rule the for-of cursor follows. The
    // generic read is `Get(array, ToString(index))`, trap and all.
    Rooted<Value> elem{isArray(target.get()) ? target.get().asObject<ArrayHeader>()->getElem(at)
                                             : rtArrayLikeElement(target, at)};
    if (rtExceptionPending()) return false;
    if (kind == Values) {
        produced = elem.get();
    } else {
        Rooted<Value> index{Value::fromDouble(static_cast<double>(at))};
        produced = makePair(index, elem);
    }
    return true;
}

bronze_fn_code rtArrayIteratorNextCode() { return arrayIterNext; }

uint64_t rtArrayValuesBuiltin(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    return makeArrayIterator(thisBits, Values, "values");
}

uint64_t rtArrayKeysBuiltin(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    return makeArrayIterator(thisBits, Keys, "keys");
}

uint64_t rtArrayEntriesBuiltin(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    return makeArrayIterator(thisBits, Entries, "entries");
}

}  // namespace bronze::runtime
