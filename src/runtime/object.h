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

    static constexpr uint32_t kInlineSlots = 4;

    static ObjectHeader* create(Heap& heap, NonMovingArena& arena, Shape* shape = nullptr);

    Value getProp(Heap& heap, Rooted<Value>& key, InlineCache* ic = nullptr);
    void setProp(Heap& heap, NonMovingArena& arena, Rooted<Value>& key, Rooted<Value>& val, InlineCache* ic = nullptr);

    Value getSlot(uint32_t index) const;
    void setSlot(uint32_t index, Value val);

    Value* slotsData() noexcept {
        return reinterpret_cast<Value*>(this + 1);
    }
    const Value* slotsData() const noexcept {
        return reinterpret_cast<const Value*>(this + 1);
    }
};

}  // namespace bronze
