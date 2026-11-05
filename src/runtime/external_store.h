#pragma once

// External ArrayBuffer storage: a bronze buffer whose bytes live in a host
// block rather than the collected heap. The mechanism sits in the runtime
// rather than in embed because GENERATED CODE reaches it too — a native
// answering `T[]` in transfer mode hands its block to bronze_native_buffer_wrap
// (native_registry.cpp), which builds a buffer over those very bytes — and the
// runtime links nothing above itself. embed.h's createExternalArrayBuffer /
// externalizeArrayBuffer / retainExternalStore / releaseExternalStore are the
// host-facing spelling of the same calls and share the registry here.
//
// A store is a plain refcounted host block, and the refcount is the ONLY
// lifetime authority: the bronze buffer's reference drops through a Deferred
// finalizer (a plain host stack, so a deleter may call into anything, another
// engine included), and every window handed out by rtExternalizeArrayBuffer is
// one more reference the host releases in its own time. Thread-local like
// every runtime registry — the home-thread rule.

#include <cstddef>
#include <cstdint>

#include "runtime/value.h"

namespace bronze {
class Heap;
struct ArrayBufferHeader;
}

namespace bronze::runtime {

struct ExternalWindow {
    uint8_t* bytes{nullptr};  // the window's first byte, or null on refusal
    uint32_t byteLength{0};
    void* store{nullptr};     // opaque refcount handle; owed one rtReleaseExternalStore
};

void rtRetainExternalStore(void* store);
void rtReleaseExternalStore(void* store);

// The bytes of a buffer (or of a view's window into one), migrated out of
// the heap on first call so they stop moving; a repeat call finds the same
// store. Refuses (null bytes, null store) a non-buffer, a detached buffer, or
// a buffer another thread's runtime externalized. ALLOCATES nothing in the
// bronze heap.
ExternalWindow rtExternalizeArrayBuffer(Value bufferOrView);

// A fresh ArrayBuffer over `bytes`, no copy; `deleter(user, bytes)` runs once
// the buffer is collected (Deferred). Bytes already backing a live store are
// SHARED — one more reference — and the redundant `deleter` runs at once.
// Null `bytes` is a TypeError, a length over kMaxByteLength a RangeError.
// Opens its own root frame; ALLOCATES.
Value rtCreateExternalArrayBuffer(uint8_t* bytes, uint32_t byteLength,
                                  void (*deleter)(void* user, uint8_t* bytes), void* user);

// Attach a newly malloc/calloc'd backing store of `byteLength` bytes to `buf`,
// registering its Deferred finalizer on `heap`. Caller must not allocate
// between creating `buf` and calling this.
//
// Every new store counts toward a full collection of the making thread's
// heap: once the external bytes that thread made since its last request pass
// the larger of 64 MB and its external bytes live before them, one is
// requested (the next allocation runs it), so a thread that holds big
// buffers but allocates little still frees them.
void rtAttachNewExternalStore(Heap& heap, ArrayBufferHeader* buf, uint8_t* bytes, uint32_t byteLength);

// Host memory a heap object keeps alive outside the heap (a native handle's
// pixels): `delta` bytes more (positive) or fewer (negative), counted with
// the calling thread's external stores toward the same full-collection
// request. Every addition must be matched by its removal when the memory is
// freed, on the thread that added it.
void rtAdjustExternalMemory(int64_t delta) noexcept;

// Number of live external stores registered on the current thread.
size_t rtExternalStoreCount() noexcept;

}  // namespace bronze::runtime
