#include "runtime/shared_store.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "runtime/heap.h"
#include "runtime/native_handle.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/typed_array.h"

namespace bronze::runtime {

namespace {

struct SharedStoreRegistry {
    std::mutex mu;
    std::unordered_map<uint64_t, SharedBufferBlock*> byAddr;
};

SharedStoreRegistry& registry() {
    // Intentionally leaked process-singleton so worker/static-teardown ordering never destroys it early.
    static auto* reg = new SharedStoreRegistry();
    return *reg;
}

void dropSharedBufferRef(void* block) { rtReleaseSharedStore(static_cast<SharedBufferBlock*>(block)); }

}  // namespace

SharedBufferBlock* rtCreateSharedStore(uint32_t byteLength, uint32_t maxByteLength, bool growable) {
    const size_t reserve = std::max<size_t>(std::max(byteLength, maxByteLength), 8);
    auto* bytes = static_cast<uint8_t*>(std::calloc(reserve, 1));
    if (!bytes) return nullptr;
    auto* block = new SharedBufferBlock();
    block->refs.store(1, std::memory_order_relaxed);
    block->bytes = bytes;
    block->byteLength.store(byteLength, std::memory_order_relaxed);
    block->maxByteLength = std::max(byteLength, maxByteLength);
    block->growable = growable;
    {
        SharedStoreRegistry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mu);
        reg.byAddr.emplace(reinterpret_cast<uint64_t>(bytes), block);
    }
    return block;
}

SharedBufferBlock* rtFindSharedStore(uint64_t bytesAddr) {
    if (bytesAddr == 0) return nullptr;
    SharedStoreRegistry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mu);
    auto it = reg.byAddr.find(bytesAddr);
    if (it == reg.byAddr.end()) return nullptr;
    it->second->refs.fetch_add(1, std::memory_order_relaxed);
    return it->second;
}

void rtRetainSharedStore(SharedBufferBlock* block) noexcept {
    if (block) block->refs.fetch_add(1, std::memory_order_relaxed);
}

void rtReleaseSharedStore(SharedBufferBlock* block) noexcept {
    if (!block) return;
    SharedStoreRegistry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mu);
    if (block->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        reg.byAddr.erase(reinterpret_cast<uint64_t>(block->bytes));
        std::free(block->bytes);
        delete block;
    }
}

bool rtGrowSharedStore(SharedBufferBlock* block, uint32_t newLen) noexcept {
    if (!block || !block->growable || newLen > block->maxByteLength) return false;
    uint32_t cur = block->byteLength.load(std::memory_order_acquire);
    while (true) {
        if (newLen < cur) return false;
        if (block->byteLength.compare_exchange_weak(cur, newLen, std::memory_order_release,
                                                    std::memory_order_acquire)) {
            return true;
        }
    }
}

void rtAttachSharedStore(Heap& heap, ArrayBufferHeader* buf, SharedBufferBlock* block) {
    buf->externalPtrBits = reinterpret_cast<uint64_t>(block->bytes);
    rtRegisterHeapFinalizer(heap, &buf->object.header, block, dropSharedBufferRef, Finalize::Deferred);
}

Value rtAdoptSharedStore(SharedBufferBlock* block) {
    if (!block) return Value::fromUndefined();
    return Value::fromObject(
        ArrayBufferHeader::createSharedOverStore(rtHeap(), rtSharedArrayBufferInstanceShape(), block));
}

}  // namespace bronze::runtime
