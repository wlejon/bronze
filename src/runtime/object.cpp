#include "runtime/object.h"

#include "runtime/fatal.h"

namespace bronze {

ObjectHeader* ObjectHeader::create(Heap& heap, NonMovingArena& arena, Shape* shape) {
    if (!shape) {
        shape = Shape::createRoot(arena);
    }
    size_t payload_bytes =
        (sizeof(ObjectHeader) - sizeof(HeapObjectHeader)) + kInlineSlots * sizeof(Value);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    auto* obj = reinterpret_cast<ObjectHeader*>(raw_hdr);
    obj->shape = shape;
    obj->overflow = Value::fromUndefined();

    Value* slots = obj->slotsData();
    for (uint32_t i = 0; i < kInlineSlots; ++i) {
        slots[i] = Value::fromUndefined();
    }
    return obj;
}

Value ObjectHeader::getSlot(uint32_t index) const {
    if (index < kInlineSlots) {
        return slotsData()[index];
    }
    uint32_t oi = index - kInlineSlots;
    if (oi >= overflowCapacity()) {
        fatal("object slot index beyond overflow capacity (corrupt shape?)");
    }
    return overflow.asObject<Value>()[oi];
}

void ObjectHeader::setSlot(uint32_t index, Value val) {
    if (index < kInlineSlots) {
        slotsData()[index] = val;
        return;
    }
    uint32_t oi = index - kInlineSlots;
    if (oi >= overflowCapacity()) {
        fatal("object slot index beyond overflow capacity (corrupt shape?)");
    }
    overflow.asObject<Value>()[oi] = val;
}

// Grow self's overflow block to hold at least `needed` out-of-line slots.
// Allocation may collect and move both the object and its old block, so
// everything is re-derived through the root after allocating.
static ObjectHeader* ensureOverflow(Heap& heap, Rooted<Value>& self, uint32_t needed) {
    auto* obj = self.get().asObject<ObjectHeader>();
    uint32_t cap = obj->overflowCapacity();
    if (needed <= cap) {
        return obj;
    }
    uint32_t new_cap = cap ? cap * 2 : ObjectHeader::kInlineSlots;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    HeapObjectHeader* block = heap.allocate(new_cap * sizeof(Value), Tag::Object);
    obj = self.get().asObject<ObjectHeader>();

    Value* slots = block->payload<Value>();
    uint32_t i = 0;
    if (obj->overflow.isPointer()) {
        Value* old_slots = obj->overflow.asObject<Value>();
        for (; i < cap; ++i) {
            slots[i] = old_slots[i];
        }
    }
    for (; i < new_cap; ++i) {
        slots[i] = Value::fromUndefined();
    }
    obj->overflow = Value::fromObject(block->payload());
    return obj;
}

Value ObjectHeader::getProp(Heap& heap, Rooted<Value>& key, InlineCache* ic) {
    (void)heap;
    if (!key.get().isString()) {
        fatal("property key must be a string");
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

ObjectHeader* ObjectHeader::setProp(Heap& heap, NonMovingArena& arena, Rooted<Value>& key,
                                    Rooted<Value>& val, InlineCache* ic) {
    if (!key.get().isString()) {
        fatal("property key must be a string");
    }
    StringHeader* prop_name = key.get().asString<StringHeader>();

    if (ic && ic->cached_shape == shape) {
        setSlot(ic->cached_slot, val.get());
        return this;
    }

    uint32_t slot = 0;
    if (shape && shape->lookupProperty(prop_name, slot)) {
        if (ic) {
            ic->cached_shape = shape;
            ic->cached_slot = slot;
        }
        setSlot(slot, val.get());
        return this;
    }

    // Property not found: shape transition, possibly growing the overflow
    // block. Growth allocates, which can move this object — operate through
    // a root from here on.
    Rooted<Value> self(Value::fromObject(this));
    uint32_t new_slot = 0;
    Shape* next_shape = shape->addProperty(arena, heap, key, new_slot);

    ObjectHeader* live = self.get().asObject<ObjectHeader>();
    if (new_slot >= kInlineSlots) {
        live = ensureOverflow(heap, self, new_slot - kInlineSlots + 1);
    }

    live->shape = next_shape;
    live->setSlot(new_slot, val.get());

    if (ic) {
        ic->cached_shape = next_shape;
        ic->cached_slot = new_slot;
    }
    return live;
}

}  // namespace bronze
