#pragma once

#include <cstdint>
#include <stdexcept>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze {

struct InlineCache {
    Shape* cached_shape{nullptr};
    uint32_t cached_slot{0};
};

struct ObjectHeader {
    HeapObjectHeader header;
    Shape* shape{nullptr};
    // Undefined, or an Object-tagged pointer to a heap block of Values
    // holding slots kInlineSlots and up. Stored as a Value so the generic
    // GC payload scan forwards it like any other slot.
    Value overflow;

    static constexpr uint32_t kInlineSlots = 4;

    static ObjectHeader* create(Heap& heap, NonMovingArena& arena, Shape* shape = nullptr);

    Value getProp(Heap& heap, Rooted<Value>& key, InlineCache* ic = nullptr);
    // May allocate (overflow growth), which can move this object; use the
    // returned pointer afterwards, not `this`.
    ObjectHeader* setProp(Heap& heap, NonMovingArena& arena, Rooted<Value>& key, Rooted<Value>& val, InlineCache* ic = nullptr);

    Value getSlot(uint32_t index) const;
    void setSlot(uint32_t index, Value val);

    uint32_t overflowCapacity() const noexcept {
        if (!overflow.isPointer()) return 0;
        const auto* hdr = HeapObjectHeader::fromPayload(overflow.asObject<void>());
        return static_cast<uint32_t>((hdr->size - sizeof(HeapObjectHeader)) / sizeof(Value));
    }

    Value* slotsData() noexcept {
        return reinterpret_cast<Value*>(this + 1);
    }
    const Value* slotsData() const noexcept {
        return reinterpret_cast<const Value*>(this + 1);
    }
};

}  // namespace bronze
