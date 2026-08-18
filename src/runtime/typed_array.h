#pragma once

#include <cstddef>
#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

// Typed arrays. The byte store and the nine views over it are one header each,
// not ten: an element kind is DATA in the view, so every path below —
// construction, the conversion on store, printing, iteration — is written once
// and reads a table.

// ECMA-262 table 71's element types. The first nine are in the order the
// specification lists them and the last three are APPENDED, which is a
// deliberate divergence: `kind` is a stored field, generated code's inline
// element access switches on the NUMBER (BRONZE_ABI_TA_KIND_* below), and the
// runtime tests compare them — so inserting Float16 and the two BigInt rows
// where 23.2 puts them would silently reinterpret every view every previously
// built object file creates. The table order is the enum's, not the
// specification's, and the specification's order is not otherwise load-bearing.
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
    // IEEE 754 binary16. Not an ABI kind: generated code's inline switch names
    // only the nine above and everything else takes the helper.
    Float16,
    // The two rows whose element values are BIGINTS rather than Numbers, which
    // is the one fact that makes them different in kind and not just in width:
    // `get`/`set` below speak `double` and CANNOT carry these, so both refuse
    // them by name and the raw-bits pair is the only road to their bytes.
    BigInt64,
    BigUint64,
    Count,
};

// Do this kind's elements read and write as BigInts? 23.2.5.13 goes through
// ToBigInt rather than ToNumber for exactly these two, which is why a Number
// written into one is a TypeError instead of a truncation.
inline constexpr bool isBigIntElementKind(ElementKind kind) noexcept {
    return kind == ElementKind::BigInt64 || kind == ElementKind::BigUint64;
}

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
    uint32_t maxByteLength;
    uint32_t bufferFlags;
    uint32_t reserved;

    static constexpr uint16_t kFlags = HeapKind::ArrayBuffer;

    static constexpr uint32_t kFlagResizable = 1U << 0;
    static constexpr uint32_t kFlagDetached = 1U << 1;
    // A SharedArrayBuffer (25.2). A FLAG and not a HeapKind of its own, and the
    // reason is that every difference between the two is in the API SURFACE
    // rather than in the storage: 25.2.5 gives a SAB `grow` and `growable`
    // where 25.1.6 gives an ArrayBuffer `resize`, `transfer` and `detached`,
    // and every view over either addresses the same bytes the same way. A kind
    // of its own would fork every `flags == ArrayBufferHeader::kFlags` test in
    // the runtime — the typed-array constructor, the DataView constructor, the
    // collector's raw-bytes handling — to no purpose.
    //
    // bronze runs ONE agent: there are no workers, no `postMessage` and no
    // second thread, so "shared" here names the brand and the API, and the
    // memory is shared with nobody. That is what makes 25.4's `wait` and
    // `notify` refusals rather than implementations (builtin_shared_memory.cpp).
    static constexpr uint32_t kFlagShared = 1U << 2;

    // Zero-filled, as 25.1.3.1 AllocateArrayBuffer requires. `byte_length` is
    // the caller's business to validate; this allocates what it is asked for.
    static ArrayBufferHeader* create(Heap& heap, uint32_t byte_length);
    static ArrayBufferHeader* createResizable(Heap& heap, uint32_t byte_length,
                                              uint32_t max_byte_length);
    // 25.2.3.1 AllocateSharedArrayBuffer. `max_byte_length` equal to
    // `byte_length` means not growable; anything larger reserves the maximum up
    // front, exactly as a resizable ArrayBuffer does, because a moving collector
    // cannot hand out a block that later moves under a view.
    static ArrayBufferHeader* createShared(Heap& heap, uint32_t byte_length,
                                           uint32_t max_byte_length);

    bool isResizable() const noexcept { return (bufferFlags & kFlagResizable) != 0; }
    bool isDetached() const noexcept { return (bufferFlags & kFlagDetached) != 0; }
    bool isShared() const noexcept { return (bufferFlags & kFlagShared) != 0; }
    void setDetached() noexcept {
        bufferFlags |= kFlagDetached;
        byteLength = 0;
    }

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
    uint32_t length;    // in ELEMENTS, not bytes — the CURRENT window, see below
    uint32_t kind;      // ElementKind
    // The length the view was CONSTRUCTED with. `length` above is the live
    // window and is maintained, not fixed: `transfer` and a shrinking
    // `resize` close a stranded view by setting it to 0, and a growing
    // `resize` reopens it to this value (refreshLength below, driven by the
    // post-mutation walk in typed_array.cpp). Keeping the truth IN the
    // length field is what lets every bounds check — helper and inline — stay
    // a single `index < length` compare instead of consulting the buffer on
    // each access. Capped at kMaxByteLength, so the scanned {kind, this}
    // word's top 16 bits stay far below a pointer tag — the same arithmetic
    // that makes the {byteOffset, length} word safe.
    uint32_t constructedLength;

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

    // 10.4.5.9 IsTypedArrayOutOfBounds for the fixed-length views bronze
    // builds: does the CONSTRUCTED window no longer lie within its buffer?
    // `transfer` zeroes the buffer's byteLength (so every non-empty view
    // fails this forever) and `resize` can shrink it out from under a view —
    // and grow it back, which restores validity, exactly as the
    // specification's witness records do. Asked from `constructedLength`
    // and not `length`, because a closed view's `length` is already 0 and
    // would answer "in bounds". The element paths never call this — their
    // `index < length` compare is kept true by refreshLength — but the
    // `byteOffset` getter (23.2.4.4 answers +0 out of bounds) still needs
    // the question. The u64 sum is not decorative: both factors can
    // individually reach kMaxByteLength.
    bool isOutOfBounds() const noexcept {
        const auto* buf = buffer.asObject<const ArrayBufferHeader>();
        return static_cast<uint64_t>(byteOffset) +
                   static_cast<uint64_t>(constructedLength) * bytesPerElement() >
               buf->byteLength;
    }

    // Re-derive the live window after the buffer's byteLength changed: the
    // constructed length while the view fits, 0 while it does not. Only
    // closeOrReopenViews (typed_array.cpp) calls this, for every view over a
    // buffer `transfer` or `resize` just mutated.
    void refreshLength() noexcept { length = isOutOfBounds() ? 0 : constructedLength; }

    // Element access with the element kind's conversions. `set` performs the
    // narrowing 23.2.5.2 SetTypedArrayFromNumber requires; the caller has
    // already done ToNumber, because that can call user code and therefore
    // allocate, and this must not.
    //
    // A NUMERIC kind only. On a BigInt64/BigUint64 view both are a named fatal
    // rather than a lossy conversion: a `double` cannot carry 2^63 - 1, so any
    // path that reaches here with one is a path that has not been taught about
    // BigInt elements, and answering approximately would hide that forever.
    double get(uint32_t index) const noexcept;
    void set(uint32_t index, double value) noexcept;

    // The eight stored bytes of a 64-bit element, in host byte order. The
    // BigInt kinds' only accessor, and deliberately raw: converting to and from
    // a BigInt VALUE allocates, and nothing in this header may.
    uint64_t rawBits64(uint32_t index) const noexcept;
    void setRawBits64(uint32_t index, uint64_t bits) noexcept;

    // A view over a fresh zero-filled buffer of `length` elements.
    static TypedArrayHeader* create(Heap& heap, ElementKind kind, uint32_t length);
    // A view over an EXISTING buffer. The buffer arrives through a root
    // because allocating the view can move it; the offset and length are the
    // caller's to validate (23.2.5.1 has the whole ladder).
    static TypedArrayHeader* createOverBuffer(Heap& heap, ElementKind kind,
                                              Rooted<Value>& buffer_val, uint32_t byteOffset,
                                              uint32_t length);
};

// Generated code inlines dynamic-index element access on float views
// (llvm_elem.cpp), so this layout — and the two float kinds' numbers — is ABI.
static_assert(offsetof(TypedArrayHeader, buffer) == BRONZE_ABI_TA_BUFFER_OFFSET);
static_assert(offsetof(TypedArrayHeader, byteOffset) == BRONZE_ABI_TA_BYTEOFFSET_OFFSET);
static_assert(offsetof(TypedArrayHeader, length) == BRONZE_ABI_TA_LENGTH_OFFSET);
static_assert(offsetof(TypedArrayHeader, kind) == BRONZE_ABI_TA_KIND_OFFSET);
static_assert(static_cast<uint32_t>(ElementKind::Int8) == BRONZE_ABI_TA_KIND_INT8);
static_assert(static_cast<uint32_t>(ElementKind::Uint8) == BRONZE_ABI_TA_KIND_UINT8);
static_assert(static_cast<uint32_t>(ElementKind::Uint8Clamped) == BRONZE_ABI_TA_KIND_UINT8CLAMPED);
static_assert(static_cast<uint32_t>(ElementKind::Int16) == BRONZE_ABI_TA_KIND_INT16);
static_assert(static_cast<uint32_t>(ElementKind::Uint16) == BRONZE_ABI_TA_KIND_UINT16);
static_assert(static_cast<uint32_t>(ElementKind::Int32) == BRONZE_ABI_TA_KIND_INT32);
static_assert(static_cast<uint32_t>(ElementKind::Uint32) == BRONZE_ABI_TA_KIND_UINT32);
static_assert(static_cast<uint32_t>(ElementKind::Float32) == BRONZE_ABI_TA_KIND_FLOAT32);
static_assert(static_cast<uint32_t>(ElementKind::Float64) == BRONZE_ABI_TA_KIND_FLOAT64);
static_assert(static_cast<uint32_t>(ElementKind::BigInt64) == BRONZE_ABI_TA_KIND_BIGINT64);
static_assert(static_cast<uint32_t>(ElementKind::BigUint64) == BRONZE_ABI_TA_KIND_BIGINT64 + 1,
              "the dynamic-store fast path discards out-of-bounds Number stores for every kind "
              "below BIGINT64 and takes the helper at or above it (the ToBigInt throw), so the "
              "two BigInt kinds must be the enum's LAST rows");
static_assert(sizeof(ArrayBufferHeader) == BRONZE_ABI_BUF_DATA_OFFSET,
              "data() is `this + 1`, so a buffer's bytes begin at sizeof(ArrayBufferHeader)");

// Close every view a buffer mutation stranded and reopen every view it
// re-admitted: refreshLength on each live TypedArrayHeader over `buffer_val`.
// Called by `transfer`, `transferToFixedLength` and `resize` AFTER the
// buffer's byteLength changed — the cold end of the bargain that keeps every
// element bounds check a single compare. Collects first (the only state in
// which the heap walks as a gapless run of fully-built objects), so the
// buffer arrives through a root.
void closeOrReopenViews(Heap& heap, Rooted<Value>& buffer_val);

// ECMA-262 25.3's DataView. A second view over the same `ArrayBufferHeader`,
// and deliberately not a tenth ElementKind: an element kind fixes a width and a
// byte order for every access, and this object's whole purpose is that
// `getFloat32(1, true)` fixes neither. So it carries no `kind` at all — the
// type is a parameter of each ACCESS, which is why the sixteen accessors are
// where the widths live and the header holds only the window.
//
// The GC rule is TypedArrayHeader's, unchanged and just as absolute: the buffer
// is a `Value` so the generic payload scan forwards it, and the data address is
// recomputed from that `Value` on every access, never cached across anything
// that can allocate.
//
// The layout obeys kMaxByteLength for the same reason the view above does.
// `{byteOffset, byteLength}` share the one 8-byte word the collector reads as a
// `Value`, and on a little-endian host `byteLength` is its top half — so the
// tag `forward_value` would test is `byteLength >> 16`. A byteLength at or
// above 0xFFF1_0000 would present a valid pointer tag and be "relocated",
// overwriting the window's length with an address. The cap is three orders of
// magnitude below that, and it binds here because a DataView can only ever be
// built over a buffer the cap already admitted. There is no third word to
// worry about: the two fields fill it, so no padding is left holding whatever
// the allocator last wrote there.
struct DataViewHeader {
    HeapObjectHeader header;
    Value buffer;
    uint32_t byteOffset;
    uint32_t byteLength;

    static constexpr uint16_t kFlags = HeapKind::DataView;

    // The first byte of this view. Valid only until the next allocation.
    uint8_t* bytes() noexcept {
        return buffer.asObject<ArrayBufferHeader>()->data() + byteOffset;
    }
    const uint8_t* bytes() const noexcept {
        return buffer.asObject<const ArrayBufferHeader>()->data() + byteOffset;
    }

    // 25.3.1's IsViewOutOfBounds: the window no longer lies within its buffer.
    // Unlike a typed array's — whose maintained `length` answers most of the
    // questions — this is asked on EVERY DataView access, because every access
    // is already a helper call and a detached or shrunk-away window is a
    // TypeError here (25.3.1.1 step 6), not a discarded read of stale bytes.
    // The detached test is not redundant with the arithmetic: `setDetached`
    // zeroes the buffer's byteLength, which the sum catches for every
    // non-empty window, but a zero-length view at offset 0 still passes it and
    // 25.1.3.4 says detached means out of bounds regardless.
    bool isOutOfBounds() const noexcept {
        const auto* buf = buffer.asObject<const ArrayBufferHeader>();
        return buf->isDetached() ||
               static_cast<uint64_t>(byteOffset) + byteLength > buf->byteLength;
    }

    // The buffer arrives through a root because allocating the view can move
    // it; the offset and length are the caller's to validate (25.3.2.1 has the
    // whole ladder).
    static DataViewHeader* create(Heap& heap, Rooted<Value>& buffer_val, uint32_t byteOffset,
                                  uint32_t byteLength);
};

static_assert(sizeof(DataViewHeader) == 24,
              "a DataView is a header, a buffer Value and ONE scanned {byteOffset, byteLength} "
              "word; a third payload word would be scanned as a Value too");

// The narrowing conversions of 7.1.6..7.1.11, exposed because construction
// from another typed array converts element by element without materialising
// a view. `value` has already been through ToNumber.
double convertForStore(ElementKind kind, double value) noexcept;

// IEEE 754 binary16, both directions, exposed because `Math.f16round` (21.3.2.26)
// is the round trip and nothing else about it is Math's business.
//
// The store direction rounds to nearest with ties to EVEN, which 6.1.6.1 makes
// the rule for every float element type, and it is computed from the double
// rather than through a `float` — a double-rounding through binary32 gives the
// wrong answer for values near a binary16 tie. A NaN stores as 0x7E00, the
// canonical quiet half, so two different input NaNs cannot be told apart by
// reading the bytes back.
uint16_t doubleToFloat16Bits(double value) noexcept;
double float16BitsToDouble(uint16_t bits) noexcept;

}  // namespace bronze
