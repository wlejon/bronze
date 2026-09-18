// External ArrayBuffer storage — the header carries the contract; this is the
// machinery, moved here from embed so that a native's transferred buffer
// (bronze_native_buffer_wrap) and a host's createExternalArrayBuffer are one
// registry with one refcount rule.

#include "runtime/external_store.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/native_handle.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/typed_array.h"

namespace bronze::runtime {

namespace {

struct ExternalStore {
    std::atomic<uint32_t> refs;
    uint8_t* bytes;
    void (*deleter)(void* user, uint8_t* bytes);
    void* user;
};

// bytes-address → store, so a repeat externalize of a buffer finds the store
// its externalPtrBits already names (the header has no second word to carry
// the store pointer itself). Entries erase when the last reference drops.
thread_local std::unordered_map<uint64_t, ExternalStore*> g_externalStores;

void freeMallocStore(void* user, uint8_t* bytes) {
    (void)user;
    std::free(bytes);
}

// The buffer's own reference, dropped when the collector proves the header
// dead. Deferred, so the release — and through it a host deleter — runs at
// the drainFinalizers checkpoint and never mid-collection.
void dropBufferRef(void* store) { rtReleaseExternalStore(store); }

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

void rtRetainExternalStore(void* store) {
    if (store) static_cast<ExternalStore*>(store)->refs.fetch_add(1, std::memory_order_relaxed);
}

void rtReleaseExternalStore(void* store) {
    if (!store) return;
    auto* s = static_cast<ExternalStore*>(store);
    if (s->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        g_externalStores.erase(reinterpret_cast<uint64_t>(s->bytes));
        if (s->deleter) s->deleter(s->user, s->bytes);
        delete s;
    }
}

ExternalWindow rtExternalizeArrayBuffer(Value bufferOrView) {
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
        rtRegisterHeapFinalizer(&buf->object.header, store, dropBufferRef, Finalize::Deferred);
    }
    rtRetainExternalStore(store);
    return {store->bytes + winOff, winLen, store};
}

Value rtCreateExternalArrayBuffer(uint8_t* bytes, uint32_t byteLength,
                                  void (*deleter)(void* user, uint8_t* bytes), void* user) {
    ShadowStackFrame frame;
    if (!bytes) {
        return rtThrowTypeError("createExternalArrayBuffer: null byte store");
    }
    if (byteLength > kMaxByteLength) {
        return rtThrowRangeError("ArrayBuffer: byte length exceeds maximum supported size");
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
        rtRetainExternalStore(store);
        if (deleter) deleter(user, bytes);
    }
    // An ordinary `ArrayBuffer` to the program — `ArrayBuffer.prototype` on its
    // chain, `instanceof ArrayBuffer` true — whose bytes happen to live in the
    // host's block. The intrinsic's instance shape says so; the bytes are the
    // external word below, and the header carries no inline byte at all.
    ArrayBufferHeader* buf =
        ArrayBufferHeader::createExternal(rtHeap(), rtArrayBufferInstanceShape(), byteLength);
    buf->externalPtrBits = reinterpret_cast<uint64_t>(bytes);
    if (!store) {
        store = new ExternalStore{{1}, bytes, deleter, user};
        g_externalStores.emplace(reinterpret_cast<uint64_t>(bytes), store);
    }
    rtRegisterHeapFinalizer(&buf->object.header, store, dropBufferRef, Finalize::Deferred);
    return Value::fromObject(buf);
}

}  // namespace bronze::runtime
