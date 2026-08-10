#include "runtime/heap.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <new>
#include <stdexcept>

namespace bronze {

constexpr uintptr_t kMaxLowAddressLimit = 1ULL << 47;

void* VirtualMemory::reserve(size_t bytes) {
#ifdef _WIN32
    void* ptr = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
    if (!ptr) {
        throw std::bad_alloc();
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr >= kMaxLowAddressLimit) {
        VirtualFree(ptr, 0, MEM_RELEASE);
        throw std::runtime_error("Heap VirtualAlloc reserved address exceeds 47-bit range");
    }
    return ptr;
#else
    void* ptr = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        throw std::bad_alloc();
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr >= kMaxLowAddressLimit) {
        munmap(ptr, bytes);
        throw std::runtime_error("Heap mmap reserved address exceeds 47-bit range");
    }
    return ptr;
#endif
}

bool VirtualMemory::commit(void* ptr, size_t bytes) {
#ifdef _WIN32
    return VirtualAlloc(ptr, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(ptr, bytes, PROT_READ | PROT_WRITE) == 0;
#endif
}

void VirtualMemory::decommit(void* ptr, size_t bytes) {
#ifdef _WIN32
    VirtualFree(ptr, bytes, MEM_DECOMMIT);
#else
    mprotect(ptr, bytes, PROT_NONE);
#endif
}

void VirtualMemory::release(void* ptr, size_t bytes) {
#ifdef _WIN32
    (void)bytes;
    if (ptr) {
        VirtualFree(ptr, 0, MEM_RELEASE);
    }
#else
    if (ptr && ptr != MAP_FAILED) {
        munmap(ptr, bytes);
    }
#endif
}

Heap::Heap(size_t reserve_bytes, size_t initial_commit_bytes)
    : reserved_bytes_(reserve_bytes) {
    reserved_base_ = VirtualMemory::reserve(reserved_bytes_);
    bump_ptr_ = static_cast<uint8_t*>(reserved_base_);

    if (initial_commit_bytes > 0) {
        size_t commit_target = std::min(initial_commit_bytes, reserved_bytes_);
        ensure_commit(commit_target);
    }
}

Heap::~Heap() {
    if (reserved_base_) {
        VirtualMemory::release(reserved_base_, reserved_bytes_);
        reserved_base_ = nullptr;
    }
}

bool Heap::ensure_commit(size_t required_bytes) {
    if (required_bytes <= committed_bytes_) {
        return true;
    }
    if (required_bytes > reserved_bytes_) {
        return false;
    }

    constexpr size_t kPageStep = 64 * 1024;
    size_t target_commit = (required_bytes + kPageStep - 1) & ~(kPageStep - 1);
    target_commit = std::min(target_commit, reserved_bytes_);
    size_t commit_size = target_commit - committed_bytes_;
    uint8_t* commit_addr = static_cast<uint8_t*>(reserved_base_) + committed_bytes_;

    if (!VirtualMemory::commit(commit_addr, commit_size)) {
        return false;
    }

    committed_bytes_ = target_commit;
    return true;
}

void Heap::collect() {
    if (collection_hook_) {
        collection_hook_(*this);
    }
}

void* Heap::allocate_raw(size_t bytes) {
    size_t aligned_bytes = (bytes + 7) & ~static_cast<size_t>(7);
    size_t current_used = used_size();
    size_t needed = current_used + aligned_bytes;

    if (needed > committed_bytes_) {
        if (!ensure_commit(needed)) {
            collect();
            if (used_size() + aligned_bytes > committed_bytes_) {
                if (!ensure_commit(used_size() + aligned_bytes)) {
                    throw std::bad_alloc();
                }
            }
        }
    }

    uint8_t* ptr = bump_ptr_;
    bump_ptr_ += aligned_bytes;
    return ptr;
}

HeapObjectHeader* Heap::allocate(size_t bytes, Tag tag) {
    size_t total_bytes = sizeof(HeapObjectHeader) + bytes;
    void* mem = allocate_raw(total_bytes);
    auto* header = static_cast<HeapObjectHeader*>(mem);
    header->tag = static_cast<uint16_t>(tag);
    header->flags = 0;
    header->size = static_cast<uint32_t>((total_bytes + 7) & ~static_cast<size_t>(7));
    return header;
}

NonMovingArena::NonMovingArena(size_t chunk_size) : chunk_size_(chunk_size) {}

NonMovingArena::~NonMovingArena() {
    for (size_t i = 0; i < chunks_.size(); ++i) {
        VirtualMemory::release(chunks_[i], chunk_capacities_[i]);
    }
}

void NonMovingArena::allocate_new_chunk(size_t min_bytes) {
    size_t capacity = std::max(chunk_size_, min_bytes);
    void* mem = VirtualMemory::reserve(capacity);
    if (!VirtualMemory::commit(mem, capacity)) {
        VirtualMemory::release(mem, capacity);
        throw std::bad_alloc();
    }
    chunks_.push_back(static_cast<uint8_t*>(mem));
    chunk_capacities_.push_back(capacity);
    current_offset_ = 0;
    current_chunk_capacity_ = capacity;
}

void* NonMovingArena::allocate(size_t bytes, size_t alignment) {
    if (alignment == 0) alignment = 8;
    if (chunks_.empty()) {
        allocate_new_chunk(bytes + alignment);
    }

    uint8_t* current = chunks_.back() + current_offset_;
    uintptr_t addr = reinterpret_cast<uintptr_t>(current);
    size_t padding = (alignment - (addr % alignment)) % alignment;

    if (current_offset_ + padding + bytes > current_chunk_capacity_) {
        allocate_new_chunk(bytes + alignment);
        current = chunks_.back();
        addr = reinterpret_cast<uintptr_t>(current);
        padding = (alignment - (addr % alignment)) % alignment;
    }

    current_offset_ += padding + bytes;
    total_allocated_ += padding + bytes;
    return current + padding;
}

}  // namespace bronze
