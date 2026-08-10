#include "runtime/array.h"

#include "runtime/fatal.h"

namespace bronze {

ArrayHeader* ArrayHeader::create(Heap& heap, uint32_t initial_capacity) {
    size_t payload_bytes =
        (sizeof(ArrayHeader) - sizeof(HeapObjectHeader)) + initial_capacity * sizeof(Value);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    auto* arr = reinterpret_cast<ArrayHeader*>(raw_hdr);
    arr->length = 0;
    arr->capacity = initial_capacity;

    Value* elems = arr->elementsData();
    for (uint32_t i = 0; i < initial_capacity; ++i) {
        elems[i] = Value::fromUndefined();
    }
    return arr;
}

Value ArrayHeader::getElem(uint32_t index) const {
    if (index >= length) {
        return Value::fromUndefined();
    }
    return elementsData()[index];
}

void ArrayHeader::setElem(Heap& heap, uint32_t index, Rooted<Value>& val) {
    (void)heap;
    if (index >= capacity) {
        fatal("Out-of-bounds / sparse array write is unsupported");
    }
    elementsData()[index] = val.get();
    if (index >= length) {
        length = index + 1;
    }
}

}  // namespace bronze
