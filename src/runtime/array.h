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

    Value getElem(uint32_t index) const;

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
