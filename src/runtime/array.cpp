#include "runtime/array.h"

#include "runtime/fatal.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"

namespace bronze {

namespace {

// Replace self's element block with one of `new_capacity` entries, copying
// the live prefix across. Allocation can collect and move both the array
// and its old block, so everything is re-derived through the root after
// the allocation.
void setCapacity(Heap& heap, Rooted<Value>& self, uint32_t new_capacity) {
    HeapObjectHeader* block = heap.allocate(new_capacity * sizeof(Value), Tag::Object);
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
        slots[i] = Value::fromUndefined();
    }

    arr->elements = Value::fromObject(block);
    arr->capacity = new_capacity;
}

}  // namespace

ArrayHeader* ArrayHeader::create(Heap& heap, uint32_t initial_capacity) {
    size_t payload_bytes = sizeof(ArrayHeader) - sizeof(HeapObjectHeader);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    auto* arr = reinterpret_cast<ArrayHeader*>(raw_hdr);
    arr->length = 0;
    arr->capacity = 0;
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

Value ArrayHeader::getElem(uint32_t index) const {
    if (index >= length) {
        return Value::fromUndefined();
    }
    Value v = elementsData()[index];
    return v.isHole() ? Value::fromUndefined() : v;
}

bool ArrayHeader::hasElem(uint32_t index) const noexcept {
    if (index >= length) return false;
    return !elementsData()[index].isHole();
}

void ArrayHeader::deleteElem(uint32_t index) noexcept {
    if (index >= length) return;
    elementsData()[index] = Value::fromHole();
}

void ArrayHeader::setLength(Heap& heap, Rooted<Value>& self, uint32_t newLength) {
    {
        ArrayHeader* arr = self.get().asObject<ArrayHeader>();
        if (newLength <= arr->length) {
            Value* slots = arr->elementsData();
            for (uint32_t i = newLength; i < arr->length; ++i) slots[i] = Value::fromHole();
            arr->length = newLength;
            return;
        }
        if (newLength > arr->capacity) setCapacity(heap, self, newLength);
    }
    // `setCapacity` allocates, so the header is re-derived through the root.
    ArrayHeader* arr = self.get().asObject<ArrayHeader>();
    Value* slots = arr->elementsData();
    for (uint32_t i = arr->length; i < newLength; ++i) slots[i] = Value::fromHole();
    arr->length = newLength;
}

void ArrayHeader::setElem(Heap& heap, uint32_t index, Rooted<Value>& val) {
    if (index > length) {
        fatal("sparse array write (index past the end) is unsupported until dictionary "
              "elements land");
    }

    Rooted<Value> self(Value::fromObject(this));
    if (index >= capacity) {
        uint32_t new_capacity = capacity ? capacity * 2 : 4;
        while (new_capacity <= index) {
            new_capacity *= 2;
        }
        setCapacity(heap, self, new_capacity);
    }

    // `this` may be stale: setCapacity allocates.
    auto* arr = self.get().asObject<ArrayHeader>();
    arr->elementsData()[index] = val.get();
    if (index >= arr->length) {
        arr->length = index + 1;
    }
}

}  // namespace bronze
