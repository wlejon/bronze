#include "runtime/object.h"

#include "runtime/fatal.h"

namespace bronze {

ObjectHeader* ObjectHeader::create(Heap& heap, NonMovingArena& arena, Shape* shape) {
    (void)arena;
    if (!shape) {
        fatal("object creation without a shape (the shape carries the prototype)");
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
    return overflow.asObject<HeapObjectHeader>()->payload<Value>()[oi];
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
    overflow.asObject<HeapObjectHeader>()->payload<Value>()[oi] = val;
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
        Value* old_slots = obj->overflow.asObject<HeapObjectHeader>()->payload<Value>();
        for (; i < cap; ++i) {
            slots[i] = old_slots[i];
        }
    }
    for (; i < new_cap; ++i) {
        slots[i] = Value::fromUndefined();
    }
    obj->overflow = Value::fromObject(block);
    return obj;
}

// A cycle here would hang the property path rather than crash it, so the
// walk is bounded and says so by name. Real chains are 1–3 links.
static constexpr uint32_t kMaxPrototypeDepth = 1000;

ObjectHeader* ObjectHeader::protoAncestor(uint32_t depth) noexcept {
    ObjectHeader* cur = this;
    for (uint32_t i = 0; i < depth; ++i) {
        if (!cur->shape) return nullptr;
        Value proto = cur->shape->prototypeValue();
        if (!proto.isObject()) return nullptr;
        auto* hdr = proto.asObject<HeapObjectHeader>();
        if (hdr->flags != 0) return nullptr;
        cur = reinterpret_cast<ObjectHeader*>(hdr);
    }
    return cur;
}

Value ObjectHeader::getProp(Heap& heap, Rooted<Value>& key, InlineCache* ic) {
    (void)heap;
    if (!key.get().isString()) {
        fatal("property key must be a string");
    }
    StringHeader* prop_name = key.get().asString<StringHeader>();

    if (ic && ic->cached_shape == shape) {
        ObjectHeader* holder = protoAncestor(ic->cached_depth);
        if (holder) {
            return holder->getSlot(ic->cached_slot);
        }
        // The chain got shorter than the cache says, which the shape check
        // should have caught: the prototype lives on the shape, so it
        // cannot change without the shape changing.
        fatal("inline cache depth outruns the prototype chain (corrupt shape?)");
    }

    // Own property first, then up the prototype chain. Nothing here
    // allocates, so these raw pointers stay valid for the whole walk.
    ObjectHeader* holder = this;
    for (uint32_t depth = 0; depth <= kMaxPrototypeDepth; ++depth) {
        uint32_t slot = 0;
        if (holder->shape && holder->shape->lookupProperty(prop_name, slot)) {
            if (ic) {
                ic->cached_shape = shape;
                ic->cached_slot = slot;
                ic->cached_depth = depth;
            }
            return holder->getSlot(slot);
        }
        ObjectHeader* next = holder->protoAncestor(1);
        if (!next) {
            return Value::fromUndefined();
        }
        holder = next;
    }
    fatal("prototype chain too deep (a cycle?)");
}

ObjectHeader* ObjectHeader::setProp(Heap& heap, NonMovingArena& arena, Rooted<Value>& key,
                                    Rooted<Value>& val, InlineCache* ic, bool enumerable) {
    if (!key.get().isString()) {
        fatal("property key must be a string");
    }
    StringHeader* prop_name = key.get().asString<StringHeader>();

    // Writes never walk the prototype chain: assignment creates an OWN
    // property (bronze has no setters), so a set-site IC only ever caches
    // depth 0.
    if (ic && ic->cached_shape == shape && ic->cached_depth == 0) {
        setSlot(ic->cached_slot, val.get());
        return this;
    }

    uint32_t slot = 0;
    if (shape && shape->lookupProperty(prop_name, slot)) {
        if (ic) {
            ic->cached_shape = shape;
            ic->cached_slot = slot;
            ic->cached_depth = 0;
        }
        setSlot(slot, val.get());
        return this;
    }

    // Property not found: shape transition, possibly growing the overflow
    // block. Growth allocates, which can move this object — operate through
    // a root from here on.
    Rooted<Value> self(Value::fromObject(this));
    uint32_t new_slot = 0;
    Shape* next_shape = shape->addProperty(arena, heap, key, new_slot, enumerable);

    ObjectHeader* live = self.get().asObject<ObjectHeader>();
    if (new_slot >= kInlineSlots) {
        live = ensureOverflow(heap, self, new_slot - kInlineSlots + 1);
    }

    live->shape = next_shape;
    live->setSlot(new_slot, val.get());

    if (ic) {
        ic->cached_shape = next_shape;
        ic->cached_slot = new_slot;
        ic->cached_depth = 0;
    }
    return live;
}

}  // namespace bronze
