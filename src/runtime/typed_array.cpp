// The byte store, the nine views, and the conversion each element kind performs
// on a store. The JS surface — constructors, members and methods — is
// builtin_typed_array*.cpp; what is here is the representation and nothing
// else.

#include "runtime/typed_array.h"

#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

#include "runtime/fatal.h"

namespace bronze {

namespace {

// ECMA-262 table 71, in the enum's order. Indexed by ElementKind, so the two
// cannot drift without the assertion below firing.
constexpr ElementKindInfo kElementKinds[] = {
    {"Int8Array", 1},         {"Uint8Array", 1},    {"Uint8ClampedArray", 1},
    {"Int16Array", 2},        {"Uint16Array", 2},   {"Int32Array", 4},
    {"Uint32Array", 4},       {"Float32Array", 4},  {"Float64Array", 8},
    {"Float16Array", 2},      {"BigInt64Array", 8}, {"BigUint64Array", 8},
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

// IEEE 754 binary16, computed from the DOUBLE's bits and never through a
// `float`. The two-step double -> float -> half rounds twice, and a value that
// is a tie in binary16 but not in binary32 comes out one ulp wrong.
//
// Every scale here is exact, because every divisor is a power of two and the
// scaled value lands in [1024, 2048) for a normal and [0, 1024] for a
// subnormal. So the only rounding in the function is the one `nearbyint`
// performs -- round-half-to-EVEN, which is 6.1.6.1's rule for a float element.
uint16_t halfBitsOf(double value) noexcept {
    if (std::isnan(value)) return 0x7E00;  // the canonical quiet half
    const uint16_t sign = std::signbit(value) ? 0x8000 : 0;
    const double a = std::fabs(value);
    if (std::isinf(a)) return static_cast<uint16_t>(sign | 0x7C00);
    if (a == 0.0) return sign;  // and this is where a -0 keeps its sign

    constexpr double kMinNormal = 6.103515625e-05;  // 2^-14
    if (a < kMinNormal) {
        // Subnormal: the quantum is 2^-24 whatever the value, so one scale and
        // one round settle it. A value that rounds UP to 1024 quanta has become
        // the smallest NORMAL half, which the exponent field then says.
        const double scaled = a * 16777216.0;  // a / 2^-24, exact
        const auto q = static_cast<uint32_t>(std::nearbyint(scaled));
        if (q >= 1024) return static_cast<uint16_t>(sign | 0x0400);
        return static_cast<uint16_t>(sign | q);
    }

    int e = 0;
    (void)std::frexp(a, &e);  // a == m * 2^e, m in [0.5, 1), so a == 1.f * 2^(e-1)
    int halfExponent = e - 1 + 15;
    const double quantum = std::ldexp(1.0, e - 1 - 10);
    auto q = static_cast<uint32_t>(std::nearbyint(a / quantum));  // 1024..2048
    if (q == 2048) {
        // The round carried into the exponent: 1.111...1 became 10.000...0.
        q = 1024;
        ++halfExponent;
    }
    // 31 is infinity's exponent field, so anything that reaches it overflowed --
    // which is every finite double at or above 65520, the tie that rounds to
    // even and therefore up.
    if (halfExponent >= 31) return static_cast<uint16_t>(sign | 0x7C00);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(halfExponent) << 10) |
                                 (q - 1024));
}

double doubleOfHalfBits(uint16_t bits) noexcept {
    const bool negative = (bits & 0x8000) != 0;
    const int exponent = (bits >> 10) & 0x1F;
    const int mantissa = bits & 0x3FF;
    double v = 0.0;
    if (exponent == 0) {
        v = std::ldexp(static_cast<double>(mantissa), -24);
    } else if (exponent == 31) {
        // A NaN read back is the value model's canonical NaN, exactly as a
        // Float32 or Float64 read is: Value::fromDouble canonicalizes, so a
        // stored NaN's payload bits are unobservable by design.
        v = mantissa != 0 ? std::numeric_limits<double>::quiet_NaN()
                          : std::numeric_limits<double>::infinity();
    } else {
        // (1 + mantissa/1024) * 2^(exponent-15), written so the significand is
        // an exact integer and the only operation is a power-of-two scale.
        v = std::ldexp(1024.0 + static_cast<double>(mantissa), exponent - 25);
    }
    return negative ? -v : v;
}

}  // namespace

uint16_t doubleToFloat16Bits(double value) noexcept { return halfBitsOf(value); }

double float16BitsToDouble(uint16_t bits) noexcept { return doubleOfHalfBits(bits); }

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
        // Float16 cannot narrow by storing, because there is no half type to
        // store into: the round trip through the bit pattern IS the narrowing,
        // and it rounds to nearest-even like the two above.
        case ElementKind::Float16: return doubleOfHalfBits(halfBitsOf(value));
        case ElementKind::BigInt64:
        case ElementKind::BigUint64:
        case ElementKind::Count: break;
    }
    fatal("internal: a store to a typed array with an element kind outside the table (a "
          "BigInt64/BigUint64 element is a BigInt and never a double; its bytes go through "
          "setRawBits64)");
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
        case ElementKind::Float16: {
            uint16_t v;
            std::memcpy(&v, p, sizeof v);
            return doubleOfHalfBits(v);
        }
        case ElementKind::BigInt64:
        case ElementKind::BigUint64:
        case ElementKind::Count: break;
    }
    fatal("internal: a read from a typed array with an element kind outside the table (a "
          "BigInt64/BigUint64 element is a BigInt and never a double; its bytes go through "
          "rawBits64)");
}

void TypedArrayHeader::set(uint32_t index, double value) noexcept {
    const ElementKind k = elementKind();
    uint8_t* p = bytes() + static_cast<size_t>(index) * bytesPerElement();
    if (k == ElementKind::Float32) {
        auto v = static_cast<float>(value);
        std::memcpy(p, &v, sizeof v);
        return;
    }
    if (k == ElementKind::Float64) {
        std::memcpy(p, &value, sizeof value);
        return;
    }
    if (k == ElementKind::Float16) {
        const uint16_t v = halfBitsOf(value);
        std::memcpy(p, &v, sizeof v);
        return;
    }
    const double c = convertForStore(k, value);
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
        case ElementKind::Float32:
        case ElementKind::Float64:
        case ElementKind::Float16:
        case ElementKind::BigInt64:
        case ElementKind::BigUint64:
        case ElementKind::Count: break;
    }
    fatal("internal: a store to a typed array with an element kind outside the table (a "
          "BigInt64/BigUint64 element is a BigInt and never a double; its bytes go through "
          "setRawBits64)");
}

uint64_t TypedArrayHeader::rawBits64(uint32_t index) const noexcept {
    if (bytesPerElement() != 8) {
        fatal("internal: a 64-bit raw read from a typed array whose elements are not 8 bytes");
    }
    uint64_t v = 0;
    std::memcpy(&v, bytes() + static_cast<size_t>(index) * 8, sizeof v);
    return v;
}

void TypedArrayHeader::setRawBits64(uint32_t index, uint64_t bits) noexcept {
    if (bytesPerElement() != 8) {
        fatal("internal: a 64-bit raw write to a typed array whose elements are not 8 bytes");
    }
    std::memcpy(bytes() + static_cast<size_t>(index) * 8, &bits, sizeof bits);
}

ArrayBufferHeader* ArrayBufferHeader::create(Heap& heap, uint32_t byte_length) {
    size_t payload_bytes = (sizeof(ArrayBufferHeader) - sizeof(HeapObjectHeader)) + byte_length;
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::RawBytes);
    auto* buf = reinterpret_cast<ArrayBufferHeader*>(raw_hdr);
    buf->header.flags = kFlags;
    buf->byteLength = byte_length;
    buf->maxByteLength = byte_length;
    buf->bufferFlags = 0;
    buf->reserved = 0;
    std::memset(buf->data(), 0, byte_length);
    return buf;
}

ArrayBufferHeader* ArrayBufferHeader::createResizable(Heap& heap, uint32_t byte_length,
                                                      uint32_t max_byte_length) {
    size_t payload_bytes =
        (sizeof(ArrayBufferHeader) - sizeof(HeapObjectHeader)) + max_byte_length;
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::RawBytes);
    auto* buf = reinterpret_cast<ArrayBufferHeader*>(raw_hdr);
    buf->header.flags = kFlags;
    buf->byteLength = byte_length;
    buf->maxByteLength = max_byte_length;
    buf->bufferFlags = kFlagResizable;
    buf->reserved = 0;
    std::memset(buf->data(), 0, max_byte_length);
    return buf;
}

ArrayBufferHeader* ArrayBufferHeader::createShared(Heap& heap, uint32_t byte_length,
                                                   uint32_t max_byte_length) {
    // The GROWABLE case reserves the maximum immediately, for the reason
    // createResizable does: a view holds a byte offset into this block and the
    // block cannot be reallocated under it.
    const uint32_t reserve = max_byte_length > byte_length ? max_byte_length : byte_length;
    size_t payload_bytes = (sizeof(ArrayBufferHeader) - sizeof(HeapObjectHeader)) + reserve;
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::RawBytes);
    auto* buf = reinterpret_cast<ArrayBufferHeader*>(raw_hdr);
    buf->header.flags = kFlags;
    buf->byteLength = byte_length;
    buf->maxByteLength = reserve;
    // `kFlagResizable` rides along when it can grow: `grow` is `resize` that
    // refuses to shrink, and one bit answering "is this block bigger than its
    // current length" keeps the two from disagreeing about the reservation.
    buf->bufferFlags = kFlagShared | (reserve > byte_length ? kFlagResizable : 0u);
    buf->reserved = 0;
    std::memset(buf->data(), 0, reserve);
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
                                                     uint32_t length, bool tracking) {
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
    view->constructedLength = tracking ? kAutoLength : length;
    return view;
}

void closeOrReopenViews(Heap& heap, Rooted<Value>& buffer_val) {
    // Collect FIRST: between collections the live space interleaves dead
    // allocations and the inline-allocation window's uninitialized bytes,
    // and a walk that misparsed one header would stomp an arbitrary object.
    // Right after a collection the space is exactly the live set, gapless
    // and fully built. Dead views over this buffer need no visit — nothing
    // can read their length again — and the collection just discarded them.
    heap.collect();
    auto* buf = buffer_val.get().asObject<ArrayBufferHeader>();
    heap.walk_objects([buf](HeapObjectHeader* hdr) {
        if (hdr->tag != static_cast<uint16_t>(Tag::Object) ||
            hdr->flags != HeapKind::TypedArray) {
            return;
        }
        auto* view = reinterpret_cast<TypedArrayHeader*>(hdr);
        if (view->buffer.asObject<ArrayBufferHeader>() != buf) return;
        view->refreshLength();
    });
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
