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

    static ArrayHeader* create(Heap& heap, uint32_t initial_capacity = 4);

    Value getElem(uint32_t index) const;
    void setElem(Heap& heap, uint32_t index, Rooted<Value>& val);

    Value* elementsData() noexcept {
        return reinterpret_cast<Value*>(this + 1);
    }
    const Value* elementsData() const noexcept {
        return reinterpret_cast<const Value*>(this + 1);
    }
};

}  // namespace bronze
