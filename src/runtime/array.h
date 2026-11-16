#pragma once

#include <cstdint>
#include <stdexcept>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

struct ObjectHeader;
class Shape;

namespace runtime {
// The shape every array starts at: a bare root (no inline Values, shape.h
// createBareRoot) over null, one per thread, published into the TLS block's
// `array_shape` for generated code's inline array allocation (rt_state.cpp).
Shape* rtArrayShape();
}  // namespace runtime

// An Array exotic object (ECMA-262 10.4.2). It begins with the object prefix
// every object value has (heap.h carriesShape): the header, the shape word and
// the overflow block, so `a.foo` is an ordinary shape-and-slot property and a
// cache entry for one is the entry a plain object's would be. The array's own
// fields follow the prefix where a plain object's inline slots would be — its
// shape is a BARE one (no inline Values), so every named property it numbers
// lives in the overflow block.
struct ArrayHeader {
    HeapObjectHeader header;
    // The array's NAMED properties — every own property that is neither an
    // element nor `length` — exactly as a plain object's are: a match array's
    // `index`, `input` and `groups` (ECMA-262 22.2.7.2 steps 16-28), an
    // `arguments` object's `callee`, and anything a program assigned.
    //
    // The shape's root is NOT the array's [[Prototype]] (heap.h
    // shapeNamesPrototype): it is null for an ordinary array, whose chain is
    // %Array.prototype% answered beside the value (the method table,
    // rt_prop.cpp), and the subclass prototype for a subclass instance or an
    // array given one by `setPrototypeOf` (kHasCustomPrototype; runtime/
    // native_base.h). The property path reads the named properties BEFORE the
    // array's own members (rt_prop_array.cpp owns every rule about them). An
    // ordinary array sits at runtime::rtArrayShape(), which is what `isBare`
    // asks.
    Shape* shape{nullptr};
    HeapValue overflow;
    uint32_t length{0};
    uint32_t capacity{0};
    uint32_t head_offset{0};
    uint32_t reserved{0};
    // Undefined, or an Object-tagged pointer to the header of a heap block
    // of `capacity` Values. Out of line so that growing an array keeps its
    // identity: only the block is reallocated, and every Value already
    // pointing at this header stays valid. (Inline elements would have to
    // move the array object itself, invalidating every reference to it.)
    HeapValue elements;

    // The array as the object its prefix makes it, for the shape machinery.
    ObjectHeader* object() noexcept { return reinterpret_cast<ObjectHeader*>(this); }
    const ObjectHeader* object() const noexcept { return reinterpret_cast<const ObjectHeader*>(this); }

    // An array with no named property, no prototype of its own and no
    // integrity level recorded: still at the thread's array shape. What every
    // fast path that answers from the elements and the method table alone asks
    // (the method cache, the push/pop fast paths, the NUMBERS claim).
    bool isBare() const { return shape == runtime::rtArrayShape(); }

    static constexpr uint32_t kHasCustomPrototype = 1;
    // NUMBERS: every element below `length` is a Number in its double form
    // (`Value::isNumber()`: no hole, no Int32 box, nothing tagged), so a
    // read in bounds is a load of a Number. Speculated code spends it to skip
    // the hole and number tests after each read (`is.dense_array` with
    // il::kDenseNumbers, il/il_op.h).
    //
    // SET by `claimNumbers`, which scans, and on an empty array generated code
    // makes (create.array of length 0, il_alloc_lowering.cpp), so the bit is
    // never more than a true answer about the elements as they are. CLEARED by
    // every write that could falsify it. The mutable accessors below clear it
    // themselves, so a path that writes elements without knowing about the bit
    // can only make the array slower, never make the bit lie; the few writers
    // that keep it (setElem, the push/pop/shift fast paths, a shrinking
    // setLength) use `elementsKeepingNumbers` and say why at the write.
    static constexpr uint32_t kElementsAreNumbers = BRONZE_ABI_ARRAY_BIT_NUMBERS;
    static_assert(kElementsAreNumbers != kHasCustomPrototype);
    // NO_CLAIM: `claimNumbers` refused this array once and will not scan it
    // again (bronze_abi_layout.h).
    static constexpr uint32_t kNumbersRefused = BRONZE_ABI_ARRAY_BIT_NO_CLAIM;

    bool elementsAreNumbers() const noexcept { return (reserved & kElementsAreNumbers) != 0; }
    void forgetNumbers() noexcept { reserved &= ~kElementsAreNumbers; }

    // Sets NUMBERS when it is true now: a bare array (isBare) whose every
    // element below `length` is a Number. An Int32-boxed element
    // (what a bitwise operator produces) is rewritten to the same Number in
    // double form first — the same value, so nothing can tell — because the
    // bit is about the representation the reads assume. Arrays longer than
    // BRONZE_ABI_ARRAY_CLAIM_MAX_LENGTH are not scanned. Returns the bit.
    bool claimNumbers() noexcept;

    // For a bare array the runtime has just made and filled (its `length`
    // elements written, none a hole): NUMBERS when every element is a Number,
    // else NO_CLAIM, so no reader's claim (il_elem_lowering.cpp) ever calls
    // out to scan it. claimNumbers' answer without its call and tests.
    void settleNumbers() noexcept {
        const Value* cur = std::as_const(*this).elementsData();
        for (uint32_t i = 0; i < length; ++i) {
            if (!cur[i].isNumber()) {
                reserved |= kNumbersRefused;
                return;
            }
        }
        reserved |= kElementsAreNumbers;
    }
    // The same for a maker that already knows the answer (a pair of values
    // in hand): no scan.
    void settleNumbers(bool allNumbers) noexcept {
        reserved |= allNumbers ? kElementsAreNumbers : kNumbersRefused;
    }

    // An ordinary array, at runtime::rtArrayShape().
    static ArrayHeader* create(Heap& heap, uint32_t initial_capacity = 4);

    // `undefined` for an index past the end AND for a HOLE — the internal
    // sentinel is never user-visible, so reading a deleted element answers
    // exactly what reading a missing property does.
    //
    // Inline (with hasElem and deleteElem below) for the reason
    // runtime/tls_block.h gives for its accessor: these are a bounds check and
    // one load, and the sort alone asks getElem three times per comparison —
    // the chunk-6 sampler charged the out-of-line pair 0.44 ms/frame of
    // `many_meshes`, all of it call overhead.
    Value getElem(uint32_t index) const {
        if (index >= length) {
            return Value::fromUndefined();
        }
        Value v = elementsData()[index];
        return v.isHole() ? Value::fromUndefined() : v;
    }

    // Whether the index is an OWN property, which a hole is not: `delete a[1]`
    // leaves `length` alone and takes index 1 out of `Object.keys`, `for-in`
    // and `in`. This is the question those three ask and `getElem` cannot
    // answer, since both a hole and a stored `undefined` read as `undefined`.
    bool hasElem(uint32_t index) const noexcept {
        if (index >= length) return false;
        return !elementsData()[index].isHole();
    }

    // `delete a[i]`. Punches a hole and leaves `length` where it was; an
    // index at or past the end was never an own property, so it is a no-op.
    void deleteElem(uint32_t index) noexcept {
        if (index >= length) return;
        elementsData()[index] = Value::fromHole();  // the mutable accessor forgets NUMBERS
    }

    // Writing at or past `length` appends and grows the block as needed,
    // filling intermediate slots with holes.
    // Growth allocates, which can move this object, so the write
    // is performed through a rooted self-reference and `this` must not be used
    // afterwards — but only on the GROWTH path: a write that lands inside the
    // current block allocates nothing, so it runs in place with no root at
    // all. The old shape opened a `Rooted` per call whether or not the call
    // could allocate, which billed every in-range store — the sort's write
    // path above all — for a defense only the growth edge needs.
    //
    // NUMBERS survives a Number written in bounds or appended at `length`:
    // every element below the new length is still a Number. Anything else
    // forgets it, here or in setElemSlow (which also forgets it for a write
    // past `length`, since that leaves holes).
    void setElem(Heap& heap, uint32_t index, Rooted<Value>& val) {
        if (head_offset + index < capacity && index <= length) {
            if (!val.get().isNumber()) forgetNumbers();
            elementsKeepingNumbers()[index] = val.get();
            if (index >= length) length = index + 1;
            return;
        }
        setElemSlow(heap, index, val);
    }

    // The same write for a caller holding a plain Value: in place when the
    // write cannot allocate, rooted only across the growth edge. Safe because
    // the fast path performs no allocation between reading `v` and storing it,
    // and the slow path roots `v` before anything can move.
    void setElem(Heap& heap, uint32_t index, Value v) {
        if (head_offset + index < capacity && index <= length) {
            if (!v.isNumber()) forgetNumbers();
            elementsKeepingNumbers()[index] = v;
            if (index >= length) length = index + 1;
            return;
        }
        Rooted<Value> val{v};
        setElemSlow(heap, index, val);
    }

    // The growth/compaction edge of setElem: past-the-end fatal, in-place
    // compaction when head headroom covers the write, block growth otherwise.
    void setElemSlow(Heap& heap, uint32_t index, Rooted<Value>& val);

    // `length` moved to exactly `newLength`, which is the storage half of
    // ArraySetLength (ECMA-262 10.4.2.4) and nothing else: the WRITABILITY of
    // `length` and the RangeError for a value that is not one are the runtime's
    // (rt_prop_array.cpp), because they are rules about the property rather
    // than about the block.
    //
    // Shrinking overwrites the dropped elements with HOLES rather than leaving
    // them, so growing back does not resurrect a value the language says was
    // deleted — and the collector stops tracing them. Growing leaves holes for
    // the same reason: nothing between the old and the new length is an own
    // property. Allocates when the block has to grow, so the array arrives
    // through a root and `this` must not be reused.
    static void setLength(Heap& heap, Rooted<Value>& self, uint32_t newLength);

    // `count` elements of `src` from `start`, appended to `self` in one copy
    // with one barrier: what `slice`, a spread of an array and `concat` are
    // when nothing between the read and the write can run a program. A hole
    // of the source arrives as a hole, or as `undefined` when
    // `holesAsUndefined` (an iteration reads it so). `start + count` must not
    // pass the source's length. NUMBERS is kept when the result is all
    // Numbers because the source was and `self` was (or was empty). Allocates,
    // so both arrive through roots; they may be the same array.
    static void appendRange(Heap& heap, Rooted<Value>& self, Rooted<Value>& src, uint32_t start,
                            uint32_t count, bool holesAsUndefined);

    // The MUTABLE accessors forget NUMBERS: whoever asks for a writable
    // pointer may store anything through it. A reader holding a non-const
    // header should call the const overload (`std::as_const`) so that reading
    // does not cost the array its bit.
    HeapValue* elementsData() noexcept {
        forgetNumbers();
        return elementsKeepingNumbers();
    }
    // The writable elements WITHOUT forgetting NUMBERS, for a writer that keeps
    // the promise itself: it stores only Numbers below `length`, or forgets
    // the bit before it stores anything else.
    HeapValue* elementsKeepingNumbers() noexcept {
        if (!elements.isPointer()) return nullptr;
        return elements.asObject<HeapObjectHeader>()->payload<HeapValue>() + head_offset;
    }
    const Value* elementsData() const noexcept {
        if (!elements.isPointer()) return nullptr;
        return rawElementsData() + head_offset;
    }
    HeapValue* rawElementsData() noexcept {
        forgetNumbers();
        if (!elements.isPointer()) return nullptr;
        return elements.asObject<HeapObjectHeader>()->payload<HeapValue>();
    }
    // The elements block, the object a bulk write into elementsData() names
    // to gcCopyValues / gcRememberObject; null when there is none.
    HeapObjectHeader* elementsBlock() const noexcept {
        return elements.isPointer() ? elements.asObject<HeapObjectHeader>() : nullptr;
    }
    const Value* rawElementsData() const noexcept {
        if (!elements.isPointer()) return nullptr;
        return elements.asObject<HeapObjectHeader>()->payload<Value>();
    }
};

static_assert(offsetof(ArrayHeader, shape) == BRONZE_ABI_OBJ_SHAPE_OFFSET);
static_assert(offsetof(ArrayHeader, overflow) == BRONZE_ABI_OBJ_OVERFLOW_OFFSET);
static_assert(offsetof(ArrayHeader, length) == BRONZE_ABI_ARRAY_LENGTH_OFFSET);
static_assert(offsetof(ArrayHeader, capacity) == BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
static_assert(offsetof(ArrayHeader, head_offset) == BRONZE_ABI_ARRAY_HEAD_OFFSET);
static_assert(offsetof(ArrayHeader, elements) == BRONZE_ABI_ARRAY_ELEMS_OFFSET);
static_assert(offsetof(ArrayHeader, reserved) == BRONZE_ABI_ARRAY_BITS_OFFSET);
static_assert(sizeof(ArrayHeader) == BRONZE_ABI_ARRAY_HEADER_BYTES);

}  // namespace bronze
