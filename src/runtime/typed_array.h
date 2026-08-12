#pragma once

#include <cstdint>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

// Typed arrays. The byte store and the nine views over it are one header each,
// not ten: an element kind is DATA in the view, so every path below —
// construction, the conversion on store, printing, iteration — is written once
// and reads a table.

// ECMA-262 table 71's element types, in the order the specification lists
// them. The order is pinned: `kind` is a stored field and the runtime tests
// compare the numbers, so inserting one in the middle would silently
// reinterpret every view a previous build produced (which matters only inside
// one run today, and is the kind of thing that stops being harmless).
enum class ElementKind : uint32_t {
    Int8 = 0,
    Uint8,
    Uint8Clamped,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Float32,
    Float64,
    Count,
};

struct ElementKindInfo {
    const char* name;      // the constructor's name, which is also what inspect prints
    uint32_t bytesPerElement;
};

const ElementKindInfo& elementKindInfo(ElementKind kind) noexcept;

// The largest byte length bronze will allocate for one buffer. Two separate
// obligations meet at this number and both are real:
//
//  - a semispace collector COPIES the buffer on every collection, so a buffer
//    larger than a semispace can never survive one; and
//  - the view header's `{byteOffset, length}` pair shares one 8-byte word,
//    which the collector's payload scan reads as a `Value`. Its top 16 bits
//    are `length >> 16`, so a length at or above 0xFFF1_0000 elements would
//    present a valid pointer TAG to `forward_value` and be "relocated" — the
//    view's length would be overwritten with an address. This cap is three
//    orders of magnitude below that, which is what makes the shared word safe
//    rather than lucky.
//
// A request above it is a RangeError, by name, at the constructor.
inline constexpr uint32_t kMaxByteLength = 1u << 28;  // 256 MiB

// Raw byte storage (Tag::RawBytes: the collector forwards the object and
// copies its bytes, but never scans the payload as Values). flags == kFlags
// discriminates it from a plain object in the dynamic helpers.
struct ArrayBufferHeader {
    HeapObjectHeader header;
    uint32_t byteLength;
    uint32_t reserved;

    static constexpr uint16_t kFlags = HeapKind::ArrayBuffer;

    // Zero-filled, as 25.1.3.1 AllocateArrayBuffer requires. `byte_length` is
    // the caller's business to validate; this allocates what it is asked for.
    static ArrayBufferHeader* create(Heap& heap, uint32_t byte_length);

    uint8_t* data() noexcept { return reinterpret_cast<uint8_t*>(this + 1); }
    const uint8_t* data() const noexcept { return reinterpret_cast<const uint8_t*>(this + 1); }
};

// A view over an ArrayBufferHeader (flags == kFlags). The buffer is held as a
// Value so the generic GC payload scan keeps it alive AND forwards it; the data
// address is recomputed from that Value on every access, never cached across
// anything that can allocate. That is the whole of the GC design and the one
// rule every method here must keep.
struct TypedArrayHeader {
    HeapObjectHeader header;
    Value buffer;
    uint32_t byteOffset;
    uint32_t length;    // in ELEMENTS, not bytes
    uint32_t kind;      // ElementKind
    uint32_t reserved;  // zero; keeps the scanned word above `kind` a non-pointer

    static constexpr uint16_t kFlags = HeapKind::TypedArray;

    ElementKind elementKind() const noexcept { return static_cast<ElementKind>(kind); }
    uint32_t bytesPerElement() const noexcept {
        return elementKindInfo(elementKind()).bytesPerElement;
    }
    uint32_t byteLength() const noexcept { return length * bytesPerElement(); }
    const char* kindName() const noexcept { return elementKindInfo(elementKind()).name; }

    // The first byte of this view. Valid only until the next allocation.
    uint8_t* bytes() noexcept {
        return buffer.asObject<ArrayBufferHeader>()->data() + byteOffset;
    }
    const uint8_t* bytes() const noexcept {
        return buffer.asObject<const ArrayBufferHeader>()->data() + byteOffset;
    }

    // Element access with the element kind's conversions. `set` performs the
    // narrowing 23.2.5.2 SetTypedArrayFromNumber requires; the caller has
    // already done ToNumber, because that can call user code and therefore
    // allocate, and this must not.
    double get(uint32_t index) const noexcept;
    void set(uint32_t index, double value) noexcept;

    // A view over a fresh zero-filled buffer of `length` elements.
    static TypedArrayHeader* create(Heap& heap, ElementKind kind, uint32_t length);
    // A view over an EXISTING buffer. The buffer arrives through a root
    // because allocating the view can move it; the offset and length are the
    // caller's to validate (23.2.5.1 has the whole ladder).
    static TypedArrayHeader* createOverBuffer(Heap& heap, ElementKind kind,
                                              Rooted<Value>& buffer_val, uint32_t byteOffset,
                                              uint32_t length);
};

// The narrowing conversions of 7.1.6..7.1.11, exposed because construction
// from another typed array converts element by element without materialising
// a view. `value` has already been through ToNumber.
double convertForStore(ElementKind kind, double value) noexcept;

}  // namespace bronze
