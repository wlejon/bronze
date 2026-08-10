#include "runtime/typed_array.h"

#include <cstring>

#include "runtime/fatal.h"

namespace bronze {

ArrayBufferHeader* ArrayBufferHeader::create(Heap& heap, uint32_t byte_length) {
    size_t payload_bytes = (sizeof(ArrayBufferHeader) - sizeof(HeapObjectHeader)) + byte_length;
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::RawBytes);
    auto* buf = reinterpret_cast<ArrayBufferHeader*>(raw_hdr);
    buf->header.flags = 4;
    buf->byteLength = byte_length;
    buf->reserved = 0;
    std::memset(buf->data(), 0, byte_length);
    return buf;
}

Float32ArrayHeader* Float32ArrayHeader::create(Heap& heap, uint32_t length) {
    // The buffer must be rooted across the view allocation.
    Rooted<Value> buf(Value::fromObject(ArrayBufferHeader::create(heap, length * 4)));
    return createOverBuffer(heap, buf);
}

Float32ArrayHeader* Float32ArrayHeader::createOverBuffer(Heap& heap, Rooted<Value>& buffer_val) {
    auto* buf_pre = buffer_val.get().asObject<ArrayBufferHeader>();
    if (buf_pre->byteLength % 4 != 0) {
        fatal("Float32Array over an ArrayBuffer whose byteLength is not a multiple of 4");
    }
    size_t payload_bytes = sizeof(Float32ArrayHeader) - sizeof(HeapObjectHeader);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    auto* view = reinterpret_cast<Float32ArrayHeader*>(raw_hdr);
    view->header.flags = 3;
    // Re-read through the root: the view allocation may have moved the buffer.
    view->buffer = buffer_val.get();
    view->byteOffset = 0;
    view->length = buffer_val.get().asObject<ArrayBufferHeader>()->byteLength / 4;
    return view;
}

}  // namespace bronze
