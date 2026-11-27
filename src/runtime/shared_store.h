#pragma once

// Process-wide reference-counted backing store for SharedArrayBuffer.
//
// Each SharedArrayBuffer allocates a heap header whose `externalPtrBits` points
// at a shared, non-moving `SharedBufferBlock`. Multiple threads (e.g. Workers)
// retain/adopt the same block and observe concurrent writes, atomic operations,
// wait/notify, and cross-thread `.grow()` through the same block.

#include <atomic>
#include <cstdint>

#include "runtime/value.h"

namespace bronze {
class Heap;
struct ArrayBufferHeader;
}

namespace bronze::runtime {

struct SharedBufferBlock {
    std::atomic<uint32_t> refs{1};
    uint8_t* bytes{nullptr};
    std::atomic<uint32_t> byteLength{0};
    uint32_t maxByteLength{0};
    bool growable{false};
};

// Allocates a zeroed SharedBufferBlock with refcount = 1 and registers it in
// the process-wide lookup map. Returns nullptr on allocation failure.
SharedBufferBlock* rtCreateSharedStore(uint32_t byteLength, uint32_t maxByteLength, bool growable);

// Looks up `bytesAddr` (`ArrayBufferHeader::externalPtrBits`) and returns it
// with refcount incremented by 1, or nullptr if not registered.
SharedBufferBlock* rtFindSharedStore(uint64_t bytesAddr);

void rtRetainSharedStore(SharedBufferBlock* block) noexcept;
void rtReleaseSharedStore(SharedBufferBlock* block) noexcept;

// Atomically grows `block->byteLength` to `newLen` if `cur <= newLen <= maxByteLength`.
// Returns true if `newLen` was accepted (including `newLen == cur`), false if `newLen < cur`
// or `newLen > maxByteLength`.
bool rtGrowSharedStore(SharedBufferBlock* block, uint32_t newLen) noexcept;

// Attaches `block` (consuming or adding no extra ref beyond the caller's ownership:
// installs the Deferred heap finalizer dropping one ref when `buf` is collected).
void rtAttachSharedStore(Heap& heap, ArrayBufferHeader* buf, SharedBufferBlock* block);

// Creates a SharedArrayBuffer header in the calling thread's heap over `block`,
// retaining one reference for that JS object.
Value rtAdoptSharedStore(SharedBufferBlock* block);

}  // namespace bronze::runtime
