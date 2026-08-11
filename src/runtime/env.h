#pragma once

#include <cstdint>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

// A scope's captured variables (docs/0007). Everything in the payload is a
// Value, so the collector's generic scan forwards the parent link and every
// slot with no special case.
struct EnvHeader {
    HeapObjectHeader header;
    Value parent;  // undefined at the outermost environment

    static constexpr uint16_t kFlags = 5;

    static EnvHeader* create(Heap& heap, Rooted<Value>& parent, uint32_t slot_count);

    uint32_t slotCount() const noexcept {
        return static_cast<uint32_t>(
            (header.size - sizeof(HeapObjectHeader) - sizeof(Value)) / sizeof(Value));
    }

    Value* slotsData() noexcept { return reinterpret_cast<Value*>(this + 1); }
    const Value* slotsData() const noexcept { return reinterpret_cast<const Value*>(this + 1); }

    // Walk `depth` parent links. A depth that runs off the chain is a
    // lowering bug, not a user error, and hard-errors.
    EnvHeader* ancestor(uint32_t depth) noexcept;
};

}  // namespace bronze
