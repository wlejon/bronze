// The byte store, the nine views, and the conversion each element kind performs
// on a store. The JS surface — constructors, members and methods — is
// builtin_typed_array*.cpp; what is here is the representation and nothing
// else.

#include "runtime/typed_array.h"

#include <cmath>
#include <cstring>
#include <iterator>

#include "runtime/fatal.h"

namespace bronze {

namespace {

// ECMA-262 table 71, in the enum's order. Indexed by ElementKind, so the two
// cannot drift without the assertion below firing.
constexpr ElementKindInfo kElementKinds[] = {
    {"Int8Array", 1},         {"Uint8Array", 1},   {"Uint8ClampedArray", 1},
    {"Int16Array", 2},        {"Uint16Array", 2},  {"Int32Array", 4},
    {"Uint32Array", 4},       {"Float32Array", 4}, {"Float64Array", 8},
};

static_assert(std::size(kElementKinds) == static_cast<size_t>(ElementKind::Count),
              "the element-kind table has drifted from the ElementKind enum");

// 7.1.6..7.1.10 ToInt8/ToUint8/ToInt16/ToUint16/ToInt32/ToUint32, which
// differ from each other only in the width and the signedness — so they are
// one function taking both, rather than six that could disagree about what
// `1e40` narrows to.
//
// Step 2 answers +0 for NaN and both infinities. Step 3 truncates the
// MATHEMATICAL value, which is why -0.4 must come out as +0 and not as -0: a -0
// stored here would read back as -0 and print as `-0`, announcing a sign that
// ToInt8 does not produce.
double toIntegerModulo(double number, int bits, bool isSigned) noexcept {
    if (!std::isfinite(number)) return 0.0;
    const double modulus = std::ldexp(1.0, bits);  // 2^bits, exactly
    // fmod is exact for finite operands, so even 1e40 narrows without any
    // rounding of its own creeping in.
    double m = std::fmod(std::trunc(number), modulus);
    if (m < 0.0) m += modulus;
    if (m == 0.0) return 0.0;  // kills a -0 that survived the fmod
    if (isSigned && m >= modulus / 2.0) m -= modulus;
    return m;
}

// 7.1.11 ToUint8Clamp. Not a truncation and not a modulo: out-of-range
// saturates, and the .5 case rounds to EVEN, which is the one place JS
// rounds differently from `Math.round`.
double toUint8Clamp(double number) noexcept {
    if (std::isnan(number)) return 0.0;
    if (number <= 0.0) return 0.0;    // also catches -0
    if (number >= 255.0) return 255.0;
    const double f = std::floor(number);
    if (f + 0.5 < number) return f + 1.0;
    if (number < f + 0.5) return f;
    return std::fmod(f, 2.0) == 0.0 ? f : f + 1.0;
}

}  // namespace

const ElementKindInfo& elementKindInfo(ElementKind kind) noexcept {
    const auto index = static_cast<uint32_t>(kind);
    if (index >= static_cast<uint32_t>(ElementKind::Count)) {
        fatal("internal: a typed array with an element kind outside the table");
    }
    return kElementKinds[index];
}

double convertForStore(ElementKind kind, double value) noexcept {
    switch (kind) {
        case ElementKind::Int8: return toIntegerModulo(value, 8, true);
        case ElementKind::Uint8: return toIntegerModulo(value, 8, false);
        case ElementKind::Uint8Clamped: return toUint8Clamp(value);
        case ElementKind::Int16: return toIntegerModulo(value, 16, true);
        case ElementKind::Uint16: return toIntegerModulo(value, 16, false);
        case ElementKind::Int32: return toIntegerModulo(value, 32, true);
        case ElementKind::Uint32: return toIntegerModulo(value, 32, false);
        // The float kinds narrow by STORING: `(float)` is IEEE round-to-
        // nearest-even, which is what 6.1.6.1 says a Float32 element does,
        // and re-widening is exact.
        case ElementKind::Float32: return static_cast<double>(static_cast<float>(value));
        case ElementKind::Float64: return value;
        case ElementKind::Count: break;
    }
    fatal("internal: a store to a typed array with an element kind outside the table");
}

double TypedArrayHeader::get(uint32_t index) const noexcept {
    const uint8_t* p = bytes() + static_cast<size_t>(index) * bytesPerElement();
    switch (elementKind()) {
        case ElementKind::Int8: {
            int8_t v;
            std::memcpy(&v, p, sizeof v);
            return v;
        }
        case ElementKind::Uint8:
        case ElementKind::Uint8Clamped: {
            uint8_t v;
            std::memcpy(&v, p, sizeof v);
            return v;
        }
        case ElementKind::Int16: {
            int16_t v;
            std::memcpy(&v, p, sizeof v);
            return v;
        }
        case ElementKind::Uint16: {
            uint16_t v;
            std::memcpy(&v, p, sizeof v);
            return v;
        }
        case ElementKind::Int32: {
            int32_t v;
            std::memcpy(&v, p, sizeof v);
            return v;
        }
        case ElementKind::Uint32: {
            uint32_t v;
            std::memcpy(&v, p, sizeof v);
            return v;
        }
        case ElementKind::Float32: {
            float v;
            std::memcpy(&v, p, sizeof v);
            return v;
        }
        case ElementKind::Float64: {
            double v;
            std::memcpy(&v, p, sizeof v);
            return v;
        }
        case ElementKind::Count: break;
    }
    fatal("internal: a read from a typed array with an element kind outside the table");
}

void TypedArrayHeader::set(uint32_t index, double value) noexcept {
    const ElementKind k = elementKind();
    const double c = convertForStore(k, value);
    uint8_t* p = bytes() + static_cast<size_t>(index) * bytesPerElement();
    switch (k) {
        case ElementKind::Int8: {
            auto v = static_cast<int8_t>(c);
            std::memcpy(p, &v, sizeof v);
            return;
        }
        case ElementKind::Uint8:
        case ElementKind::Uint8Clamped: {
            auto v = static_cast<uint8_t>(c);
            std::memcpy(p, &v, sizeof v);
            return;
        }
        case ElementKind::Int16: {
            auto v = static_cast<int16_t>(c);
            std::memcpy(p, &v, sizeof v);
            return;
        }
        case ElementKind::Uint16: {
            auto v = static_cast<uint16_t>(c);
            std::memcpy(p, &v, sizeof v);
            return;
        }
        case ElementKind::Int32: {
            auto v = static_cast<int32_t>(c);
            std::memcpy(p, &v, sizeof v);
            return;
        }
        case ElementKind::Uint32: {
            auto v = static_cast<uint32_t>(c);
            std::memcpy(p, &v, sizeof v);
            return;
        }
        case ElementKind::Float32: {
            auto v = static_cast<float>(c);
            std::memcpy(p, &v, sizeof v);
            return;
        }
        case ElementKind::Float64: {
            std::memcpy(p, &c, sizeof c);
            return;
        }
        case ElementKind::Count: break;
    }
    fatal("internal: a store to a typed array with an element kind outside the table");
}

ArrayBufferHeader* ArrayBufferHeader::create(Heap& heap, uint32_t byte_length) {
    size_t payload_bytes = (sizeof(ArrayBufferHeader) - sizeof(HeapObjectHeader)) + byte_length;
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::RawBytes);
    auto* buf = reinterpret_cast<ArrayBufferHeader*>(raw_hdr);
    buf->header.flags = kFlags;
    buf->byteLength = byte_length;
    buf->reserved = 0;
    std::memset(buf->data(), 0, byte_length);
    return buf;
}

TypedArrayHeader* TypedArrayHeader::create(Heap& heap, ElementKind kind, uint32_t length) {
    const uint32_t bpe = elementKindInfo(kind).bytesPerElement;
    // The buffer must be rooted across the view's own allocation.
    Rooted<Value> buf(Value::fromObject(ArrayBufferHeader::create(heap, length * bpe)));
    return createOverBuffer(heap, kind, buf, 0, length);
}

TypedArrayHeader* TypedArrayHeader::createOverBuffer(Heap& heap, ElementKind kind,
                                                     Rooted<Value>& buffer_val, uint32_t byteOffset,
                                                     uint32_t length) {
    size_t payload_bytes = sizeof(TypedArrayHeader) - sizeof(HeapObjectHeader);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    auto* view = reinterpret_cast<TypedArrayHeader*>(raw_hdr);
    view->header.flags = kFlags;
    // Read the buffer through the ROOT, after the allocation above: that
    // allocation may have moved it, and a copy taken before it would name
    // dead from-space.
    view->buffer = buffer_val.get();
    view->byteOffset = byteOffset;
    view->length = length;
    view->kind = static_cast<uint32_t>(kind);
    view->reserved = 0;
    return view;
}

DataViewHeader* DataViewHeader::create(Heap& heap, Rooted<Value>& buffer_val, uint32_t byteOffset,
                                       uint32_t byteLength) {
    size_t payload_bytes = sizeof(DataViewHeader) - sizeof(HeapObjectHeader);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    auto* view = reinterpret_cast<DataViewHeader*>(raw_hdr);
    view->header.flags = kFlags;
    // Read the buffer through the ROOT, after the allocation above, for the
    // reason createOverBuffer does: that allocation may have moved it.
    view->buffer = buffer_val.get();
    view->byteOffset = byteOffset;
    view->byteLength = byteLength;
    return view;
}

}  // namespace bronze
