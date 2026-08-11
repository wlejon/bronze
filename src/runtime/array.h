#pragma once

#include <cstdint>
#include <stdexcept>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

struct ArrayHeader {
    HeapObjectHeader header;
    uint32_t length{0};
    uint32_t capacity{0};
    // Undefined, or an Object-tagged pointer to the header of a heap block
    // of `capacity` Values. Out of line so that growing an array keeps its
    // identity: only the block is reallocated, and every Value already
    // pointing at this header stays valid. (Inline elements would have to
    // move the array object itself, invalidating every reference to it.)
    Value elements;

    static ArrayHeader* create(Heap& heap, uint32_t initial_capacity = 4);

    // `undefined` for an index past the end AND for a HOLE — the internal
    // sentinel is never user-visible (docs/0004), so reading a deleted
    // element answers exactly what reading a missing property does.
    Value getElem(uint32_t index) const;

    // Whether the index is an OWN property, which a hole is not: `delete
    // a[1]` leaves `length` alone and takes index 1 out of `Object.keys`,
    // `for-in` and `in` (docs/0019 decision 2). This is the question those
    // three ask and `getElem` cannot answer, since both a hole and a stored
    // `undefined` read as `undefined`.
    bool hasElem(uint32_t index) const noexcept;

    // `delete a[i]`. Punches a hole and leaves `length` where it was; an
    // index at or past the end was never an own property, so it is a no-op.
    void deleteElem(uint32_t index) noexcept;

    // Writing at `length` appends and grows the block as needed. Writing
    // past `length` is a sparse write and a named hard error until
    // dictionary elements land (docs/0004). Growth allocates, which can
    // move this object, so the write is performed through a rooted
    // self-reference and `this` must not be used afterwards.
    void setElem(Heap& heap, uint32_t index, Rooted<Value>& val);

    Value* elementsData() noexcept {
        if (!elements.isPointer()) return nullptr;
        return elements.asObject<HeapObjectHeader>()->payload<Value>();
    }
    const Value* elementsData() const noexcept {
        if (!elements.isPointer()) return nullptr;
        return elements.asObject<HeapObjectHeader>()->payload<Value>();
    }
};

}  // namespace bronze
