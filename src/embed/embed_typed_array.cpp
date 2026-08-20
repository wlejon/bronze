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

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "embed/embed.h"
#include "embed/embed_internal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
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
// against the enumeration that actually lives in every view's header. One
// assert per kind rather than a count, so the failure names the kind that moved.
static_assert(elements::Int8 == ElementKind::Int8);
static_assert(elements::Uint8 == ElementKind::Uint8);
static_assert(elements::Uint8Clamped == ElementKind::Uint8Clamped);
static_assert(elements::Int16 == ElementKind::Int16);
static_assert(elements::Uint16 == ElementKind::Uint16);
static_assert(elements::Int32 == ElementKind::Int32);
static_assert(elements::Uint32 == ElementKind::Uint32);
static_assert(elements::Float32 == ElementKind::Float32);
static_assert(elements::Float64 == ElementKind::Float64);
static_assert(elements::Float16 == ElementKind::Float16);
static_assert(elements::BigInt64 == ElementKind::BigInt64);
static_assert(elements::BigUint64 == ElementKind::BigUint64);

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

// ---- external buffer storage ------------------------------------------------
//
// embed.h carries the contract; this is the machinery. A store is a plain
// refcounted host block, and the refcount is the ONLY lifetime authority —
// the bronze buffer's reference drops through a Deferred finalizer (a plain
// host stack, so a host deleter may call into anything, another engine
// included), and every ExternalBytes handed out is one more reference the
// host releases in its own time. Nothing here touches either collector's
// rules, which is the point of the design.

namespace {

struct ExternalStore {
    std::atomic<uint32_t> refs;
    uint8_t* bytes;
    void (*deleter)(void* user, uint8_t* bytes);
    void* user;
};

// bytes-address → store, so a repeat externalize of a buffer finds the store
// its externalPtrBits already names (the header has no second word to carry
// the store pointer itself). Thread-local like every runtime registry — the
// home-thread rule — and entries erase when the last reference drops.
thread_local std::unordered_map<uint64_t, ExternalStore*> g_externalStores;

void freeMallocStore(void* user, uint8_t* bytes) {
    (void)user;
    std::free(bytes);
}

// The buffer's own reference, dropped when the collector proves the header
// dead. Deferred, so the release — and through it a host deleter — runs at
// the drainFinalizers checkpoint and never mid-collection.
void dropBufferRef(void* store) { releaseExternalStore(store); }

ArrayBufferHeader* bufferBehind(Value v) {
    if (!v.isObject()) return nullptr;
    auto* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags == HeapKind::ArrayBuffer) return reinterpret_cast<ArrayBufferHeader*>(hdr);
    if (hdr->flags == HeapKind::TypedArray) {
        return reinterpret_cast<TypedArrayHeader*>(hdr)->buffer.asObject<ArrayBufferHeader>();
    }
    return nullptr;
}

}  // namespace

void retainExternalStore(void* store) {
    if (store) static_cast<ExternalStore*>(store)->refs.fetch_add(1, std::memory_order_relaxed);
}

void releaseExternalStore(void* store) {
    if (!store) return;
    auto* s = static_cast<ExternalStore*>(store);
    if (s->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        g_externalStores.erase(reinterpret_cast<uint64_t>(s->bytes));
        if (s->deleter) s->deleter(s->user, s->bytes);
        delete s;
    }
}

ExternalBytes externalizeArrayBuffer(Value bufferOrView) {
    ArrayBufferHeader* buf = bufferBehind(bufferOrView);
    if (!buf || buf->isDetached()) return {};

    // The BUFFER is what externalizes; the answered window is the view's own,
    // so two views over one buffer cross as two windows on one store.
    uint32_t winOff = 0;
    uint32_t winLen = buf->byteLength;
    if (bufferOrView.asObject<HeapObjectHeader>()->flags == HeapKind::TypedArray) {
        auto* view =
            reinterpret_cast<TypedArrayHeader*>(bufferOrView.asObject<HeapObjectHeader>());
        winOff = view->byteOffset;
        winLen = view->byteLength();
    }

    ExternalStore* store = nullptr;
    if (buf->externalPtrBits) {
        auto it = g_externalStores.find(buf->externalPtrBits);
        // An external word this thread's registry does not know is a buffer
        // from another thread's runtime — not this call's to retain.
        if (it == g_externalStores.end()) return {};
        store = it->second;
    } else {
        // Migrate the whole RESERVATION, not just the live window, so a
        // resizable buffer's later grow finds its zeroed bytes exactly where
        // the inline layout had them and `resize` keeps working unchanged.
        const uint32_t capacity = buf->maxByteLength;
        auto* bytes = static_cast<uint8_t*>(std::malloc(capacity ? capacity : 1));
        if (!bytes) return {};
        std::memcpy(bytes, buf->data(), capacity);
        store = new ExternalStore{{1}, bytes, freeMallocStore, nullptr};
        g_externalStores.emplace(reinterpret_cast<uint64_t>(bytes), store);
        buf->externalPtrBits = reinterpret_cast<uint64_t>(bytes);
        // No bronze allocation between reading the header's address and the
        // registration — malloc is the host's heap, not this one.
        registerHeapFinalizer(&buf->header, store, dropBufferRef, Finalize::Deferred);
    }
    retainExternalStore(store);
    return {store->bytes + winOff, winLen, store};
}

Value createExternalArrayBuffer(uint8_t* bytes, uint32_t byteLength,
                                void (*deleter)(void* user, uint8_t* bytes), void* user) {
    ShadowStackFrame frame;
    if (!bytes) {
        return runtime::rtThrowTypeError("createExternalArrayBuffer: null byte store");
    }
    if (byteLength > kMaxByteLength) {
        return runtime::rtThrowRangeError(
            "ArrayBuffer: byte length exceeds maximum supported size");
    }
    // Bytes already backing a live store: the new buffer SHARES it — one more
    // reference on the same block — rather than racing it for a second
    // registration the registry could not tell apart. This is not a
    // hypothetical: a bridge whose bronze buffer over interpreter bytes died
    // at a collection re-crosses the SAME interpreter buffer before the
    // deferred drain has released the old store, and the second crossing must
    // be a fresh buffer over the still-live block. The caller's `deleter` is
    // redundant with the registration that governs the bytes, so it runs NOW
    // — its resources must not wait on a lifetime it does not own.
    ExternalStore* store = nullptr;
    if (auto it = g_externalStores.find(reinterpret_cast<uint64_t>(bytes));
        it != g_externalStores.end()) {
        store = it->second;
        retainExternalStore(store);
        if (deleter) deleter(user, bytes);
    }
    size_t payload_bytes = sizeof(ArrayBufferHeader) - sizeof(HeapObjectHeader);
    HeapObjectHeader* raw_hdr = runtime::rtHeap().allocate(payload_bytes, Tag::RawBytes);
    auto* buf = reinterpret_cast<ArrayBufferHeader*>(raw_hdr);
    buf->header.flags = ArrayBufferHeader::kFlags;
    buf->byteLength = byteLength;
    buf->maxByteLength = byteLength;
    buf->bufferFlags = 0;
    buf->reserved = 0;
    buf->externalPtrBits = reinterpret_cast<uint64_t>(bytes);
    if (!store) {
        store = new ExternalStore{{1}, bytes, deleter, user};
        g_externalStores.emplace(reinterpret_cast<uint64_t>(bytes), store);
    }
    registerHeapFinalizer(&buf->header, store, dropBufferRef, Finalize::Deferred);
    return Value::fromObject(buf);
}

Value createTypedArrayView(ElementKind kind, Value buffer, uint32_t byteOffset,
                           uint32_t length) {
    ShadowStackFrame frame;
    if (static_cast<uint32_t>(kind) >= static_cast<uint32_t>(ElementKind::Count)) {
        return runtime::rtThrowRangeError("createTypedArrayView: unknown element kind");
    }
    if (!isArrayBuffer(buffer)) {
        return runtime::rtThrowTypeError("createTypedArrayView: not an ArrayBuffer");
    }
    auto* buf = buffer.asObject<ArrayBufferHeader>();
    const uint64_t bpe = elementKindInfo(kind).bytesPerElement;
    const uint64_t end = byteOffset + static_cast<uint64_t>(length) * bpe;
    // 23.2.5.1's ladder: element alignment, then the window against the
    // buffer's CURRENT byteLength.
    if (byteOffset % bpe != 0 || end > buf->byteLength) {
        return runtime::rtThrowRangeError(
            "createTypedArrayView: window does not fit the buffer");
    }
    Rooted<Value> root{buffer};
    return Value::fromObject(TypedArrayHeader::createOverBuffer(runtime::rtHeap(), kind, root,
                                                                byteOffset, length));
}

Value typedArrayBuffer(Value view) {
    if (!isTypedArray(view)) return Value::fromUndefined();
    return reinterpret_cast<TypedArrayHeader*>(view.asObject<HeapObjectHeader>())->buffer;
}

uint32_t typedArrayByteOffset(Value view) {
    if (!isTypedArray(view)) return 0;
    return reinterpret_cast<TypedArrayHeader*>(view.asObject<HeapObjectHeader>())->byteOffset;
}

}  // namespace bronze::embed
