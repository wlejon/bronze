// Typed arrays and ArrayBuffers across the host boundary: reading the
// program's bytes in place (the seam a GL binding uploads through) and
// building a view the host fills for the program to read.
//
// The READERS allocate nothing, root nothing and open no frame: each is a read
// of a header the caller's Value already names, and keeping them
// allocation-free is what makes the answered pointer usable at all. The
// pointer contract is embed.h's, repeated because it is the entire risk of
// this file: `data` points into the moving semispace heap and dies at the NEXT
// allocation — the caller consumes it synchronously or copies it out, never
// stores it. fillTypedArray is a reader by that measure too: it copies INTO a
// pointer it derives and never lets anything allocate in between.

#include <cstring>
#include <string>

#include "embed/embed.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_state.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::embed {

TypedArrayInfo typedArrayInfo(Value v) {
    if (!v.isObject()) return {};
    auto* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::TypedArray) return {};
    auto* view = reinterpret_cast<TypedArrayHeader*>(hdr);
    // bytes() recomputes the address from the buffer Value it holds, so this
    // is the buffer's CURRENT location — current, that is, until the caller
    // lets anything allocate.
    return TypedArrayInfo{view->bytes(), view->byteLength(), view->length,
                          view->bytesPerElement(), view->elementKind()};
}

ArrayBufferInfo arrayBufferInfo(Value v) {
    if (!v.isObject()) return {};
    auto* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::ArrayBuffer) return {};
    auto* buf = reinterpret_cast<ArrayBufferHeader*>(hdr);
    return ArrayBufferInfo{buf->data(), buf->byteLength};
}

Value createArrayBuffer(size_t byteLength) {
    if (byteLength > kMaxByteLength) {
        ShadowStackFrame frame;
        return runtime::rtThrowRangeError("ArrayBuffer: byte length exceeds maximum supported size");
    }
    ShadowStackFrame frame;
    auto* buf = ArrayBufferHeader::create(runtime::rtHeap(), static_cast<uint32_t>(byteLength));
    return Value::fromObject(buf);
}

Value createArrayBuffer(std::span<const uint8_t> bytes) {
    if (bytes.size() > kMaxByteLength) {
        ShadowStackFrame frame;
        return runtime::rtThrowRangeError("ArrayBuffer: byte length exceeds maximum supported size");
    }
    ShadowStackFrame frame;
    auto* buf = ArrayBufferHeader::create(runtime::rtHeap(), static_cast<uint32_t>(bytes.size()));
    if (!bytes.empty()) {
        std::memcpy(buf->data(), bytes.data(), bytes.size());
    }
    return Value::fromObject(buf);
}

// The kind constants embed.h spells for a host that includes only that header,
// against the enumeration that actually lives in every view's header. Nine
// asserts rather than a count, so the failure names the kind that moved.
static_assert(elements::Int8 == ElementKind::Int8);
static_assert(elements::Uint8 == ElementKind::Uint8);
static_assert(elements::Uint8Clamped == ElementKind::Uint8Clamped);
static_assert(elements::Int16 == ElementKind::Int16);
static_assert(elements::Uint16 == ElementKind::Uint16);
static_assert(elements::Int32 == ElementKind::Int32);
static_assert(elements::Uint32 == ElementKind::Uint32);
static_assert(elements::Float32 == ElementKind::Float32);
static_assert(elements::Float64 == ElementKind::Float64);

Value createTypedArray(ElementKind kind, uint32_t length) {
    ShadowStackFrame frame;
    if (static_cast<uint32_t>(kind) >= static_cast<uint32_t>(ElementKind::Count)) {
        return runtime::rtThrowRangeError("createTypedArray: unknown element kind");
    }
    // The constructor's own ladder, in the same order and with the same
    // messages: the byte size in 64 bits first (so the multiply cannot wrap
    // into a small request), then the per-buffer cap, then whether a semispace
    // can actually hold it. A host asking for a gigabyte must get the
    // RangeError the program would, not a heap that dies mid-copy.
    const uint64_t bpe = elementKindInfo(kind).bytesPerElement;
    const uint64_t byteLength = static_cast<uint64_t>(length) * bpe;
    const size_t semispace = runtime::rtHeap().reserved_size() / 2;
    if (byteLength >= kMaxByteLength || byteLength + 64 >= semispace) {
        return runtime::rtThrowRangeError(
            "Array buffer allocation failed: " + std::to_string(byteLength) +
            " bytes does not fit in the heap");
    }
    return Value::fromObject(
        TypedArrayHeader::create(runtime::rtHeap(), kind, length));
}

bool fillTypedArray(Value view, std::span<const uint8_t> bytes) {
    if (!view.isObject()) return false;
    auto* hdr = view.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::TypedArray) return false;
    auto* ta = reinterpret_cast<TypedArrayHeader*>(hdr);
    if (bytes.size() > ta->byteLength()) return false;
    if (bytes.empty()) return true;
    // bytes() is recomputed from the buffer Value here, at the last possible
    // moment, and nothing between it and the memcpy can allocate — which is
    // the only reason a raw destination pointer is safe to hold at all.
    std::memcpy(ta->bytes(), bytes.data(), bytes.size());
    return true;
}

bool isArrayBuffer(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::ArrayBuffer;
}

bool isTypedArray(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::TypedArray;
}

}  // namespace bronze::embed
