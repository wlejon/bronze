#pragma once

// Binary data on the embed surface: raw views over the program's typed arrays
// and ArrayBuffers, typed arrays the host builds and fills, and buffers whose
// bytes live outside the moving heap. A companion of embed.h, carrying
// BRONZE_EMBED_API for the same reason it does; embed.h includes it, so a
// host that includes embed.h has all of it.

#include <cstddef>
#include <cstdint>
#include <span>

#include "embed/embed.h"

namespace bronze::embed {

// ---- typed-array access (embed_typed_array.cpp) ----------------------------

// Raw views over the program's binary data, for a host that consumes it in
// place — a GL buffer upload, a texture image, an audio block.
//
// THE POINTER CONTRACT, stated as loudly as it deserves: `data` points INTO
// THE MOVING BRONZE HEAP and is valid only until the next allocation on it —
// any embed call marked ALLOCATES, any call into compiled code, any native
// function a callback re-enters. A host either consumes the bytes
// synchronously (hand them to a GL call that copies them into the driver) or
// memcpy's them out before doing anything else. It never stores the pointer,
// not even alongside a Persistent — the Persistent keeps the VALUE alive and
// current, but this pointer is a snapshot of an address the collector is free
// to abandon.

struct TypedArrayInfo {
    uint8_t* data{nullptr};  // nullptr: the value was not a typed array
    uint32_t byteLength{0};
    uint32_t elementCount{0};
    uint32_t bytesPerElement{0};
    ElementKind elementKind{};  // meaningful only when data != nullptr
    explicit operator bool() const { return data != nullptr; }
};

// The view's window over its buffer — offset already applied, so `data` is
// element 0. Answers a null-data result for anything that is not a typed
// array (an ArrayBuffer and a DataView included: each has its own accessor
// or deliberately none, below).
BRONZE_EMBED_API TypedArrayInfo typedArrayInfo(Value v);

struct ArrayBufferInfo {
    uint8_t* data{nullptr};  // nullptr: the value was not an ArrayBuffer
    uint32_t byteLength{0};
    explicit operator bool() const { return data != nullptr; }
};

// The whole byte store of an ArrayBuffer value. bronze models the buffer and
// its views as separate heap kinds (runtime/typed_array.h), so a host handed
// a Float32Array must go through typedArrayInfo — this answers null for a
// view, exactly as typedArrayInfo answers null for a bare buffer. A DataView
// has no accessor here on purpose: nothing a host binding consumes arrives as
// one, and exposing it would be surface without a caller.
BRONZE_EMBED_API ArrayBufferInfo arrayBufferInfo(Value v);

// Allocate a fresh zero-filled ArrayBuffer of `byteLength` bytes. ALLOCATES.
BRONZE_EMBED_API Value createArrayBuffer(size_t byteLength);

// Allocate an ArrayBuffer initialized with a copy of `bytes`. ALLOCATES.
BRONZE_EMBED_API Value createArrayBuffer(std::span<const uint8_t> bytes);

// ---- typed-array construction (embed_typed_array.cpp) ----------------------
//
// The write half of the seam above: a host that PRODUCES binary data for the
// program — a decoded image, a mesh the engine built, a block of audio — makes
// the view itself and hands it over, rather than asking the program to
// allocate one and filling it afterwards.
//
// The element kind is the same enumeration typedArrayInfo reports, spelled
// here so a host that includes only this header can name one without pulling
// in runtime/typed_array.h. The values are pinned against that header by
// static_asserts in embed_typed_array.cpp — the enum's numbering is stored
// data (an ElementKind lives in every view's header), so it could not move
// even if these constants did not exist.
namespace elements {
inline constexpr ElementKind Int8 = static_cast<ElementKind>(0);
inline constexpr ElementKind Uint8 = static_cast<ElementKind>(1);
inline constexpr ElementKind Uint8Clamped = static_cast<ElementKind>(2);
inline constexpr ElementKind Int16 = static_cast<ElementKind>(3);
inline constexpr ElementKind Uint16 = static_cast<ElementKind>(4);
inline constexpr ElementKind Int32 = static_cast<ElementKind>(5);
inline constexpr ElementKind Uint32 = static_cast<ElementKind>(6);
inline constexpr ElementKind Float32 = static_cast<ElementKind>(7);
inline constexpr ElementKind Float64 = static_cast<ElementKind>(8);
// Appended in the runtime's enumeration order and not 23.2's, because the
// numbers are ABI for generated code and could not be renumbered. A host that
// creates one of the last two gets a view whose ELEMENTS are BigInts; the
// byte-level `fillTypedArray` works on it like any other, and there is no
// double-based host accessor for a 64-bit integer element by design.
inline constexpr ElementKind Float16 = static_cast<ElementKind>(9);
inline constexpr ElementKind BigInt64 = static_cast<ElementKind>(10);
inline constexpr ElementKind BigUint64 = static_cast<ElementKind>(11);
}  // namespace elements

// A view of `length` elements over a fresh zero-filled buffer of its own —
// `new Float32Array(n)` spelled from the host, and the same object the program
// would have got from that expression: same prototype, same element paths, and
// indistinguishable to the compiled code that receives it.
//
// A length whose byte size exceeds what bronze will allocate for one buffer is
// the RangeError the constructor raises, thrown as throwRangeError throws.
// ALLOCATES.
BRONZE_EMBED_API Value createTypedArray(ElementKind kind, uint32_t length);

// Fill from raw bytes: `bytes` is copied into the view's storage starting at
// element 0, in the HOST's byte order and layout — a memcpy, not a conversion,
// so the caller's buffer must already hold the element type's bit patterns
// (float for Float32, int32_t for Int32). This is the fast path a decoder or a
// mesh builder wants; per-element conversion from a JS number is setElement's
// job.
//
// Refuses, writing nothing, if `view` is not a typed array or if `bytes` does
// not fit — a partial fill would leave the program holding half a texture with
// nothing to distinguish it from a whole one. Does NOT allocate, and therefore
// cannot move anything: the copy is the whole of it.
BRONZE_EMBED_API bool fillTypedArray(Value view, std::span<const uint8_t> bytes);

// ---- external buffer storage (embed_typed_array.cpp) ------------------------
//
// The exception to the pointer contract above, bought deliberately: a buffer
// whose bytes live OUTSIDE the moving heap, in a refcounted host block that
// never moves. This is what lets a second engine in the same process — bro's
// QuickJS realm — hold a view over the SAME bytes a compiled program's
// Float32Array reads, instead of a copy that diverges on the first write.
// The runtime never creates one on its own; a buffer becomes external only
// through the two calls below, and every element path (interpreted helper and
// inline generated code alike) selects on the buffer's external word, which
// is the entire runtime cost.
//
// LIFETIME. The store is refcounted, and the count is the only lifetime
// authority — deliberately independent of BOTH collectors, which is what
// makes it safe where a cross-heap reference cannot be. The bronze buffer
// object holds one reference, dropped through a Deferred finalizer (so the
// release runs on a plain host stack at the drainFinalizers checkpoint, where
// a host deleter may do anything); every ExternalBytes handed out below is
// one more, released by the host with releaseExternalStore. The bytes pointer
// stays valid until the LAST reference drops, wherever that happens.

struct ExternalBytes {
    uint8_t* data{nullptr};   // nullptr: the value was not an externalizable buffer
    uint32_t byteLength{0};
    void* store{nullptr};     // opaque refcount handle; owed one releaseExternalStore
    explicit operator bool() const { return data != nullptr; }
};

// Make `bufferOrView`'s storage external, migrating the bytes out of the
// moving heap on the first call (a resizable buffer migrates its whole
// reservation, so `resize` keeps working) — idempotent after that. Accepts an
// ArrayBuffer or any typed-array view (the view's BUFFER is what
// externalizes; the answered window is the view's own). Answers null-data for
// a detached buffer or a non-buffer. The result carries a RETAINED store
// reference the caller must eventually release. ALLOCATES nothing on the
// bronze heap; the returned pointer is NOT subject to the moving-heap
// contract and survives every collection.
BRONZE_EMBED_API ExternalBytes externalizeArrayBuffer(Value bufferOrView);

// A fresh ArrayBuffer whose storage IS `bytes` — host memory bronze never
// copies, for the reverse crossing: an interpreter's buffer read in place by
// compiled code. `deleter(user, bytes)` runs when the last reference drops,
// possibly from the deferred-finalizer drain (a plain host stack; any call is
// legal there, including into another engine). bronze holds the one initial
// reference; the caller keeps none unless it retains. `bytes` must stay valid
// and fixed until the deleter runs, and byteLength is capped like any
// buffer's. If `bytes` already backs a live external store, the new buffer
// SHARES that store and `deleter` runs immediately — the existing
// registration governs the block's lifetime. ALLOCATES (the header cell).
BRONZE_EMBED_API Value createExternalArrayBuffer(uint8_t* bytes, uint32_t byteLength,
                                                 void (*deleter)(void* user, uint8_t* bytes),
                                                 void* user);

// One more / one fewer reference on a store from the two calls above.
BRONZE_EMBED_API void retainExternalStore(void* store);
BRONZE_EMBED_API void releaseExternalStore(void* store);

// A view over an EXISTING buffer — `new Float32Array(buffer, byteOffset, n)`
// spelled from the host, and the way a host hands compiled code a window onto
// an external buffer it just created. `byteOffset` and `length` are validated
// against the buffer (the constructor's RangeError, thrown as throwRangeError throws,
// on a miss); `buffer` must be an ArrayBuffer value. ALLOCATES.
BRONZE_EMBED_API Value createTypedArrayView(ElementKind kind, Value buffer,
                                            uint32_t byteOffset, uint32_t length);

// The buffer behind a typed-array view (undefined for anything else), and the
// view's byte offset into it. A bridge needs the buffer's IDENTITY — two
// views over one buffer must cross as two windows on one store, not two
// stores — and typedArrayInfo deliberately answers only the window's bytes.
BRONZE_EMBED_API Value typedArrayBuffer(Value view);
BRONZE_EMBED_API uint32_t typedArrayByteOffset(Value view);
BRONZE_EMBED_API void detachArrayBuffer(Value v);
BRONZE_EMBED_API bool isDetachedArrayBuffer(Value v);

}  // namespace bronze::embed
