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

struct ArrayHeader {
    HeapObjectHeader header;
    uint32_t length{0};
    uint32_t capacity{0};
    uint32_t head_offset{0};
    uint32_t reserved{0};
    // Undefined, or an Object-tagged pointer to the header of a heap block
    // of `capacity` Values. Out of line so that growing an array keeps its
    // identity: only the block is reallocated, and every Value already
    // pointing at this header stays valid. (Inline elements would have to
    // move the array object itself, invalidating every reference to it.)
    Value elements;
    // Undefined, or a plain object holding this array's NAMED properties —
    // every own property of the array that is neither an element nor `length`.
    //
    // An array is an object, so `a.foo = 1` is ordinary JavaScript and three.js
    // writes one on the render path; the storage was already here for the
    // runtime's own use, since a match array is an array of captures that also
    // carries `index`, `input` and `groups` (ECMA-262 22.2.7.2 steps 16-28) and
    // an `arguments` object carries `callee`. Created only when something writes
    // one, so an ordinary array is the same size it was and the element path
    // loads nothing extra.
    //
    // The object has a NULL prototype: it is storage, not a link in the array's
    // chain, and the property path reads it before the array's own members
    // (rt_prop_array.cpp owns every rule about what is in here).
    Value properties;

    static ArrayHeader* create(Heap& heap, uint32_t initial_capacity = 4);

    // The named-property object, created if this is the first one. Allocates,
    // so the array arrives through a root and `this` must not be reused.
    static ObjectHeader* ensureProperties(Heap& heap, NonMovingArena& arena,
                                          Rooted<Value>& self);

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
        elementsData()[index] = Value::fromHole();
    }

    // Writing at `length` appends and grows the block as needed. Writing past
    // `length` is a sparse write and a named hard error until dictionary
    // elements land. Growth allocates, which can move this object, so the write
    // is performed through a rooted self-reference and `this` must not be used
    // afterwards — but only on the GROWTH path: a write that lands inside the
    // current block allocates nothing, so it runs in place with no root at
    // all. The old shape opened a `Rooted` per call whether or not the call
    // could allocate, which billed every in-range store — the sort's write
    // path above all — for a defense only the growth edge needs.
    void setElem(Heap& heap, uint32_t index, Rooted<Value>& val) {
        if (head_offset + index < capacity && index <= length) {
            elementsData()[index] = val.get();
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
            elementsData()[index] = v;
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

    Value* elementsData() noexcept {
        if (!elements.isPointer()) return nullptr;
        return rawElementsData() + head_offset;
    }
    const Value* elementsData() const noexcept {
        if (!elements.isPointer()) return nullptr;
        return rawElementsData() + head_offset;
    }
    Value* rawElementsData() noexcept {
        if (!elements.isPointer()) return nullptr;
        return elements.asObject<HeapObjectHeader>()->payload<Value>();
    }
    const Value* rawElementsData() const noexcept {
        if (!elements.isPointer()) return nullptr;
        return elements.asObject<HeapObjectHeader>()->payload<Value>();
    }
};

static_assert(offsetof(ArrayHeader, length) == BRONZE_ABI_ARRAY_LENGTH_OFFSET);
static_assert(offsetof(ArrayHeader, capacity) == BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
static_assert(offsetof(ArrayHeader, head_offset) == BRONZE_ABI_ARRAY_HEAD_OFFSET);
static_assert(offsetof(ArrayHeader, elements) == BRONZE_ABI_ARRAY_ELEMS_OFFSET);
static_assert(offsetof(ArrayHeader, properties) == BRONZE_ABI_ARRAY_PROPS_OFFSET);

}  // namespace bronze
