#pragma once

#include <cstdint>
#include <stdexcept>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze {

// A monomorphic property cache: three plain words, none of which the
// collector has to touch. `cached_depth` is how many prototype links to
// follow from the receiver before reading `cached_slot` — 0 for an own
// property, so this one form covers own hits and proto hits alike. The
// holder is derived from the (non-moving) shape chain rather than cached,
// which is what keeps the entry GC-free; see docs/0008 decision 2.
struct InlineCache {
    Shape* cached_shape{nullptr};
    uint32_t cached_slot{0};
    uint32_t cached_depth{0};
};

struct ObjectHeader {
    HeapObjectHeader header;
    Shape* shape{nullptr};
    // Undefined, or an Object-tagged pointer to the HEADER of a heap block
    // of Values holding slots kInlineSlots and up. Stored as a Value so the
    // generic GC payload scan forwards it like any other slot; header, not
    // payload, because every heap reference in a Value points at a header.
    Value overflow;

    static constexpr uint32_t kInlineSlots = 4;

    // `shape` is required: it decides the object's prototype (docs/0008
    // decision 1), and minting a fresh root shape per object would give
    // every `{}` literal an unrelated hidden class.
    static ObjectHeader* create(Heap& heap, NonMovingArena& arena, Shape* shape);

    // The object `cached_depth` prototype links up from this one, or null
    // if the chain is shorter than that. No allocation, so the raw pointer
    // is safe to the next allocation.
    ObjectHeader* protoAncestor(uint32_t depth) noexcept;

    Value getProp(Heap& heap, Rooted<Value>& key, InlineCache* ic = nullptr);
    // May allocate (overflow growth), which can move this object; use the
    // returned pointer afterwards, not `this`.
    ObjectHeader* setProp(Heap& heap, NonMovingArena& arena, Rooted<Value>& key, Rooted<Value>& val, InlineCache* ic = nullptr);

    Value getSlot(uint32_t index) const;
    void setSlot(uint32_t index, Value val);

    uint32_t overflowCapacity() const noexcept {
        if (!overflow.isPointer()) return 0;
        const auto* hdr = overflow.asObject<HeapObjectHeader>();
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
