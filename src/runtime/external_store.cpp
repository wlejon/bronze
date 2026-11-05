// External ArrayBuffer storage — the header carries the contract; this is the
// machinery, moved here from embed so that a native's transferred buffer
// (bronze_native_buffer_wrap) and a host's createExternalArrayBuffer are one
// registry with one refcount rule.

#include "runtime/external_store.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
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

// External bytes are memory the collector does not see: a buffer's header is
// a few words of heap, its block can be 100 MB, and the block is freed only
// when a collection finds the header dead. A thread that allocates little on
// the heap (a Worker decoding images it transfers on) would never collect,
// and keep every block it was ever handed. So the blocks count, per thread:
// once the bytes a thread made since its last request pass the larger of
// kExternalMinBytes and what that thread had live before them (the trigger
// doubling with its live set), a full collection of its heap is requested,
// which its next allocation runs.
//
// Per thread, because only that thread's collection frees what it made: a
// process-wide total lets two Workers' garbage hold each other's trigger off
// for good (each one's new bytes never pass everything live). A block is
// debited to the thread that made it whichever thread frees it (a Worker's
// pixels transferred to the page), through the counter the store carries,
// which outlives its thread.
constexpr uint64_t kExternalMinBytes = 64ull << 20;

struct ExternalCounter {
    std::atomic<uint64_t> live{0};
};

thread_local std::shared_ptr<ExternalCounter> t_externalCounter = std::make_shared<ExternalCounter>();
thread_local uint64_t t_externalSinceRequest = 0;

void noteExternalAdded(Heap& heap, uint64_t bytes) {
    const uint64_t live = t_externalCounter->live.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    t_externalSinceRequest += bytes;
    const uint64_t before = live > t_externalSinceRequest ? live - t_externalSinceRequest : 0;
    if (t_externalSinceRequest > std::max(kExternalMinBytes, before)) {
        t_externalSinceRequest = 0;
        heap.gc().request_collection(brass::gc::CollectionKind::Full);
    }
}

// Clamped at zero: a host's adjustExternalMemory removal may come on a thread
// other than the one that added it.
void noteExternalRemoved(ExternalCounter& counter, uint64_t bytes) {
    uint64_t cur = counter.live.load(std::memory_order_relaxed);
    while (!counter.live.compare_exchange_weak(cur, cur > bytes ? cur - bytes : 0,
                                               std::memory_order_relaxed)) {
    }
}

struct ExternalStore {
    std::atomic<uint32_t> refs;
    uint8_t* bytes;
    void (*deleter)(void* user, uint8_t* bytes);
    void* user;
    uint32_t length;  // the block's bytes, for the pressure above
    std::shared_ptr<ExternalCounter> owner = t_externalCounter;  // the making thread's
};

void noteNewStore(Heap& heap, const ExternalStore* store) { noteExternalAdded(heap, store->length); }

void noteStoreFreed(const ExternalStore* store) { noteExternalRemoved(*store->owner, store->length); }

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
        noteStoreFreed(s);
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
        store = new ExternalStore{{1}, bytes, freeMallocStore, nullptr, capacity};
        g_externalStores.emplace(reinterpret_cast<uint64_t>(bytes), store);
        buf->externalPtrBits = reinterpret_cast<uint64_t>(bytes);
        // No bronze allocation between reading the header's address and the
        // registration — malloc is the host's heap, not this one.
        rtRegisterHeapFinalizer(&buf->object.header, store, dropBufferRef, Finalize::Deferred);
        noteNewStore(rtHeap(), store);
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
    const bool fresh = !store;
    if (fresh) {
        store = new ExternalStore{{1}, bytes, deleter, user, byteLength};
        g_externalStores.emplace(reinterpret_cast<uint64_t>(bytes), store);
    }
    rtRegisterHeapFinalizer(&buf->object.header, store, dropBufferRef, Finalize::Deferred);
    if (fresh) noteNewStore(rtHeap(), store);
    return Value::fromObject(buf);
}

void rtAttachNewExternalStore(Heap& heap, ArrayBufferHeader* buf, uint8_t* bytes, uint32_t byteLength) {
    auto* store = new ExternalStore{{1}, bytes, freeMallocStore, nullptr, byteLength};
    g_externalStores.emplace(reinterpret_cast<uint64_t>(bytes), store);
    buf->externalPtrBits = reinterpret_cast<uint64_t>(bytes);
    rtRegisterHeapFinalizer(heap, &buf->object.header, store, dropBufferRef, Finalize::Deferred);
    noteNewStore(heap, store);
}

void rtAdjustExternalMemory(int64_t delta) noexcept {
    if (delta > 0) noteExternalAdded(rtHeap(), static_cast<uint64_t>(delta));
    else if (delta < 0) noteExternalRemoved(*t_externalCounter, static_cast<uint64_t>(-delta));
}

size_t rtExternalStoreCount() noexcept { return g_externalStores.size(); }

}  // namespace bronze::runtime
