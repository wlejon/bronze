#include "runtime/object.h"

#include <cstdlib>
#include <iostream>

namespace bronze {

ObjectHeader* ObjectHeader::create(Heap& heap, NonMovingArena& arena, Shape* shape) {
    if (!shape) {
        shape = Shape::createRoot(arena);
    }
    size_t payload_bytes = kInlineSlots * sizeof(Value);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    auto* obj = reinterpret_cast<ObjectHeader*>(raw_hdr);
    obj->shape = shape;

    Value* slots = obj->slotsData();
    for (uint32_t i = 0; i < kInlineSlots; ++i) {
        slots[i] = Value::fromUndefined();
    }
    return obj;
}

Value ObjectHeader::getSlot(uint32_t index) const {
    if (index >= kInlineSlots) {
        std::cerr << "Hard runtime error: Out-of-line object slots and dictionary mode are unsupported" << std::endl;
        std::abort();
    }
    return slotsData()[index];
}

void ObjectHeader::setSlot(uint32_t index, Value val) {
    if (index >= kInlineSlots) {
        std::cerr << "Hard runtime error: Out-of-line object slots and dictionary mode are unsupported" << std::endl;
        std::abort();
    }
    slotsData()[index] = val;
}

Value ObjectHeader::getProp(Heap& heap, Rooted<Value>& key, InlineCache* ic) {
    (void)heap;
    if (!key.get().isString()) {
        std::cerr << "Hard runtime error: Property key must be a string" << std::endl;
        std::abort();
    }
    StringHeader* prop_name = key.get().asString<StringHeader>();

    if (ic && ic->cached_shape == shape) {
        return getSlot(ic->cached_slot);
    }

    uint32_t slot = 0;
    if (shape && shape->lookupProperty(prop_name, slot)) {
        if (ic) {
            ic->cached_shape = shape;
            ic->cached_slot = slot;
        }
        return getSlot(slot);
    }

    return Value::fromUndefined();
}

void ObjectHeader::setProp(Heap& heap, NonMovingArena& arena, Rooted<Value>& key, Rooted<Value>& val, InlineCache* ic) {
    if (!key.get().isString()) {
        std::cerr << "Hard runtime error: Property key must be a string" << std::endl;
        std::abort();
    }
    StringHeader* prop_name = key.get().asString<StringHeader>();

    if (ic && ic->cached_shape == shape) {
        setSlot(ic->cached_slot, val.get());
        return;
    }

    uint32_t slot = 0;
    if (shape && shape->lookupProperty(prop_name, slot)) {
        if (ic) {
            ic->cached_shape = shape;
            ic->cached_slot = slot;
        }
        setSlot(slot, val.get());
        return;
    }

    // Property not found, transition to new shape
    uint32_t new_slot = 0;
    Shape* next_shape = shape->addProperty(arena, heap, key, new_slot);
    if (new_slot >= kInlineSlots) {
        std::cerr << "Hard runtime error: Dictionary transition / out-of-line slots are unsupported" << std::endl;
        std::abort();
    }

    shape = next_shape;
    setSlot(new_slot, val.get());

    if (ic) {
        ic->cached_shape = shape;
        ic->cached_slot = new_slot;
    }
}

}  // namespace bronze
