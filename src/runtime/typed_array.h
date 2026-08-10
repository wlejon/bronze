#pragma once

#include <cstdint>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

// Raw byte storage (Tag::RawBytes: the GC forwards the object but never
// scans its payload as Values). header.flags == 4 discriminates it from
// plain objects in the dynamic helpers.
struct ArrayBufferHeader {
    HeapObjectHeader header;
    uint32_t byteLength;
    uint32_t reserved;

    static ArrayBufferHeader* create(Heap& heap, uint32_t byte_length);

    uint8_t* data() noexcept { return reinterpret_cast<uint8_t*>(this + 1); }
    const uint8_t* data() const noexcept { return reinterpret_cast<const uint8_t*>(this + 1); }
};

// A view over an ArrayBufferHeader (header.flags == 3). The buffer is held
// as a Value so the generic GC payload scan keeps it alive and forwards it.
struct Float32ArrayHeader {
    HeapObjectHeader header;
    Value buffer;
    uint32_t byteOffset;
    uint32_t length;  // element count

    // Fresh zeroed buffer of `length` elements.
    static Float32ArrayHeader* create(Heap& heap, uint32_t length);
    // View over an existing whole buffer (byteLength must be a multiple of 4).
    static Float32ArrayHeader* createOverBuffer(Heap& heap, Rooted<Value>& buffer_val);

    float* data() noexcept {
        auto* buf = buffer.asObject<ArrayBufferHeader>();
        return reinterpret_cast<float*>(buf->data() + byteOffset);
    }
};

}  // namespace bronze
