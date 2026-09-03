#include "runtime/array.h"

#include <cstddef>

#include "abi/bronze_abi.h"

#include "runtime/fatal.h"
#include "runtime/object.h"
#include "runtime/rt_state.h"

namespace bronze {

namespace {

// Replace self's element block with one of `new_capacity` entries, copying
// the live prefix across. Allocation can collect and move both the array
// and its old block, so everything is re-derived through the root after
// the allocation.
// What generated code's inline array literal (llvm_construct.cpp) writes has
// to be exactly what `ArrayHeader::create` + `setCapacity` write.
static_assert(sizeof(ArrayHeader) == BRONZE_ABI_ARRAY_HEADER_BYTES);
static_assert(offsetof(ArrayHeader, length) == BRONZE_ABI_ARRAY_LENGTH_OFFSET);
static_assert(offsetof(ArrayHeader, capacity) == BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
static_assert(offsetof(ArrayHeader, head_offset) == BRONZE_ABI_ARRAY_HEAD_OFFSET);
static_assert(offsetof(ArrayHeader, elements) == BRONZE_ABI_ARRAY_ELEMS_OFFSET);
static_assert(offsetof(ArrayHeader, properties) == BRONZE_ABI_ARRAY_PROPS_OFFSET);
static_assert(HeapKind::ValueBlock == BRONZE_ABI_OBJ_FLAGS_VALUE_BLOCK);
static_assert(HeapKind::Array == BRONZE_ABI_OBJ_FLAGS_ARRAY);

void setCapacity(Heap& heap, Rooted<Value>& self, uint32_t new_capacity) {
    HeapObjectHeader* block = heap.allocate(new_capacity * sizeof(Value), Tag::Object);
    // Not the zero `Heap::allocate` leaves behind, which reads as
    // `HeapKind::Plain` — and a plain object is a thing the collector reads a
    // `Shape*` out of. Elements are Values and nothing else, which is what
    // `ValueBlock` says.
    block->flags = HeapKind::ValueBlock;
    auto* arr = self.get().asObject<ArrayHeader>();

    Value* slots = block->payload<Value>();
    uint32_t i = 0;
    const Value* old_slots = arr->elementsData();
    if (old_slots) {
        for (; i < arr->length && i < new_capacity; ++i) {
            slots[i] = old_slots[i];
        }
    }
    for (; i < new_capacity; ++i) {
        slots[i] = Value::fromHole();
    }

    arr->elements = Value::fromObject(block);
    arr->capacity = new_capacity;
    arr->head_offset = 0;
}

}  // namespace

ArrayHeader* ArrayHeader::create(Heap& heap, uint32_t initial_capacity) {
    size_t payload_bytes = sizeof(ArrayHeader) - sizeof(HeapObjectHeader);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    raw_hdr->flags = HeapKind::Array;
    auto* arr = reinterpret_cast<ArrayHeader*>(raw_hdr);
    arr->length = 0;
    arr->capacity = 0;
    arr->head_offset = 0;
    arr->reserved = 0;
    arr->elements = Value::fromUndefined();
    arr->properties = Value::fromUndefined();

    if (initial_capacity > 0) {
        Rooted<Value> self(Value::fromObject(arr));
        setCapacity(heap, self, initial_capacity);
        arr = self.get().asObject<ArrayHeader>();
    }
    return arr;
}

ObjectHeader* ArrayHeader::ensureProperties(Heap& heap, NonMovingArena& arena,
                                            Rooted<Value>& self) {
    if (self.get().asObject<ArrayHeader>()->properties.isObject()) {
        return self.get().asObject<ArrayHeader>()->properties.asObject<ObjectHeader>();
    }
    // A root shape with NO prototype, memoized so that every array's bucket
    // shares one transition tree rather than minting a hidden class per match.
    //
    // Not `rtPlainObjectShape`, whose prototype is `Object.prototype`: this
    // object is storage and not a link in the array's chain, and the property
    // path reads it BEFORE the array's own members (rt_prop.cpp). With
    // `Object.prototype` behind it the bucket answered every name that object
    // carries, so a match array's `.constructor` was the `Object` namespace
    // instead of `Array`, and `.valueOf` shadowed the array's own answer. An
    // array reaches `Object.prototype` through `Array.prototype`, which is a
    // separate gap; borrowing it here put the intrinsic in the wrong position
    // rather than filling that gap.
    ObjectHeader* props =
        ObjectHeader::create(heap, arena, runtime::rtRootShapeForPrototype(Value::fromNull()));
    props->header.flags = HeapKind::Plain;
    self.get().asObject<ArrayHeader>()->properties = Value::fromObject(props);
    return props;
}

void ArrayHeader::setLength(Heap& heap, Rooted<Value>& self, uint32_t newLength) {
    {
        ArrayHeader* arr = self.get().asObject<ArrayHeader>();
        if (newLength <= arr->length) {
            Value* slots = arr->elementsData();
            for (uint32_t i = newLength; i < arr->length; ++i) slots[i] = Value::fromHole();
            arr->length = newLength;
            if (newLength == 0) {
                arr->head_offset = 0;
            }
            return;
        }
        if (arr->head_offset + newLength > arr->capacity) {
            setCapacity(heap, self, newLength);
        }
    }
    // `setCapacity` allocates, so the header is re-derived through the root.
    ArrayHeader* arr = self.get().asObject<ArrayHeader>();
    Value* slots = arr->elementsData();
    for (uint32_t i = arr->length; i < newLength; ++i) slots[i] = Value::fromHole();
    arr->length = newLength;
}

void ArrayHeader::setElemSlow(Heap& heap, uint32_t index, Rooted<Value>& val) {
    if (index > length) {
        fatal("sparse array write (index past the end) is unsupported until dictionary "
              "elements land");
    }

    Rooted<Value> self(Value::fromObject(this));
    if (head_offset + index >= capacity) {
        // If there is head headroom and in-place compaction fits the write, compact to index 0
        if (head_offset > 0 && index + 1 <= capacity) {
            Value* raw = rawElementsData();
            const Value* cur = elementsData();
            std::memmove(raw, cur, length * sizeof(Value));
            for (uint32_t i = length; i < capacity; ++i) {
                raw[i] = Value::fromHole();
            }
            head_offset = 0;
        } else {
            uint32_t new_capacity = capacity ? capacity * 2 : 4;
            while (new_capacity <= index) {
                new_capacity *= 2;
            }
            setCapacity(heap, self, new_capacity);
        }
    }

    // `this` may be stale: setCapacity allocates.
    auto* arr = self.get().asObject<ArrayHeader>();
    arr->elementsData()[index] = val.get();
    if (index >= arr->length) {
        arr->length = index + 1;
    }
}

}  // namespace bronze
