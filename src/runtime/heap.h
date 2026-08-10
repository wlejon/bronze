#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "runtime/value.h"

namespace bronze {

struct HeapObjectHeader {
    uint16_t tag;
    uint16_t flags;
    uint32_t size;

    template <typename T = void>
    T* payload() noexcept {
        return reinterpret_cast<T*>(this + 1);
    }

    template <typename T = void>
    const T* payload() const noexcept {
        return reinterpret_cast<const T*>(this + 1);
    }

    static HeapObjectHeader* fromPayload(void* ptr) noexcept {
        return reinterpret_cast<HeapObjectHeader*>(ptr) - 1;
    }
};

static_assert(sizeof(HeapObjectHeader) == 8, "HeapObjectHeader must be 8 bytes");

class VirtualMemory {
public:
    static void* reserve(size_t bytes);
    static bool commit(void* ptr, size_t bytes);
    static void decommit(void* ptr, size_t bytes);
    static void release(void* ptr, size_t bytes);
};

class Heap {
public:
    using CollectionHook = std::function<void(Heap&)>;

    explicit Heap(size_t reserve_bytes = 1024 * 1024 * 1024, size_t initial_commit_bytes = 64 * 1024);
    ~Heap();

    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    HeapObjectHeader* allocate(size_t bytes, Tag tag);
    void* allocate_raw(size_t bytes);

    void set_collection_hook(CollectionHook hook) { collection_hook_ = std::move(hook); }
    void collect();

    uintptr_t base_address() const noexcept { return reinterpret_cast<uintptr_t>(reserved_base_); }
    size_t reserved_size() const noexcept { return reserved_bytes_; }
    size_t committed_size() const noexcept { return committed_bytes_; }
    size_t used_size() const noexcept {
        return bump_ptr_ - static_cast<const uint8_t*>(reserved_base_);
    }

private:
    bool ensure_commit(size_t required_bytes);

    void* reserved_base_{nullptr};
    size_t reserved_bytes_{0};
    size_t committed_bytes_{0};
    uint8_t* bump_ptr_{nullptr};
    CollectionHook collection_hook_;
};

class NonMovingArena {
public:
    explicit NonMovingArena(size_t chunk_size = 64 * 1024);
    ~NonMovingArena();

    NonMovingArena(const NonMovingArena&) = delete;
    NonMovingArena& operator=(const NonMovingArena&) = delete;

    void* allocate(size_t bytes, size_t alignment = 8);

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }

    size_t chunk_count() const noexcept { return chunks_.size(); }
    size_t total_allocated_bytes() const noexcept { return total_allocated_; }

private:
    void allocate_new_chunk(size_t min_bytes);

    size_t chunk_size_;
    size_t current_offset_{0};
    size_t current_chunk_capacity_{0};
    size_t total_allocated_{0};
    std::vector<uint8_t*> chunks_;
    std::vector<size_t> chunk_capacities_;
};

}  // namespace bronze
