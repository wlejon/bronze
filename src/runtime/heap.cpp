#define _CRT_SECURE_NO_WARNINGS

#include "runtime/heap.h"
#include "runtime/gc.h"

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
#include <cstdlib>
#include <cstring>
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
    semispace_size_ = reserved_bytes_ / 2;

    from_space_.base = static_cast<uint8_t*>(reserved_base_);
    from_space_.size = semispace_size_;
    from_space_.committed_bytes = 0;
    from_space_.bump_ptr = from_space_.base;

    to_space_.base = static_cast<uint8_t*>(reserved_base_) + semispace_size_;
    to_space_.size = semispace_size_;
    to_space_.committed_bytes = 0;
    to_space_.bump_ptr = to_space_.base;

    if (initial_commit_bytes > 0) {
        size_t commit_target = std::min(initial_commit_bytes, semispace_size_);
        ensure_commit(from_space_, commit_target);
    }

    const char* env_stress = std::getenv("BRONZE_GC_STRESS");
    if (env_stress && (std::strcmp(env_stress, "1") == 0 ||
                       std::strcmp(env_stress, "true") == 0 ||
                       std::strcmp(env_stress, "ON") == 0)) {
        gc_stress_mode_ = true;
    }
}

Heap::~Heap() {
    if (reserved_base_) {
        VirtualMemory::release(reserved_base_, reserved_bytes_);
        reserved_base_ = nullptr;
    }
}

bool Heap::ensure_commit(Semispace& space, size_t required_bytes) {
    if (required_bytes <= space.committed_bytes) {
        return true;
    }
    if (required_bytes > space.size) {
        return false;
    }

    constexpr size_t kPageStep = 64 * 1024;
    size_t target_commit = (required_bytes + kPageStep - 1) & ~(kPageStep - 1);
    target_commit = std::min(target_commit, space.size);
    size_t commit_size = target_commit - space.committed_bytes;
    uint8_t* commit_addr = space.base + space.committed_bytes;

    if (!VirtualMemory::commit(commit_addr, commit_size)) {
        return false;
    }

    space.committed_bytes = target_commit;
    return true;
}

void* Heap::allocate_in_space(Semispace& space, size_t bytes) {
    size_t aligned_bytes = (bytes + 7) & ~static_cast<size_t>(7);
    size_t current_used = space.bump_ptr - space.base;
    size_t needed = current_used + aligned_bytes;

    if (needed > space.size) {
        throw std::bad_alloc();
    }

    if (needed > space.committed_bytes) {
        if (!ensure_commit(space, needed)) {
            throw std::bad_alloc();
        }
    }

    uint8_t* ptr = space.bump_ptr;
    space.bump_ptr += aligned_bytes;
    return ptr;
}

void* Heap::allocate_raw(size_t bytes) {
    if (gc_stress_mode_ && !in_gc_) {
        collect();
    }

    size_t aligned_bytes = (bytes + 7) & ~static_cast<size_t>(7);
    size_t current_used = from_space_.bump_ptr - from_space_.base;
    size_t needed = current_used + aligned_bytes;

    if (needed > from_space_.size || needed > from_space_.committed_bytes) {
        if (!ensure_commit(from_space_, needed)) {
            if (!in_gc_) {
                collect();
                current_used = from_space_.bump_ptr - from_space_.base;
                needed = current_used + aligned_bytes;
                if (!ensure_commit(from_space_, needed)) {
                    throw std::bad_alloc();
                }
            } else {
                throw std::bad_alloc();
            }
        }
    }

    uint8_t* ptr = from_space_.bump_ptr;
    from_space_.bump_ptr += aligned_bytes;
    return ptr;
}

HeapObjectHeader* Heap::allocate(size_t bytes, Tag tag) {
    if (gc_stress_mode_ && !in_gc_) {
        collect();
    }

    size_t total_bytes = sizeof(HeapObjectHeader) + bytes;
    void* mem = allocate_raw(total_bytes);
    auto* header = static_cast<HeapObjectHeader*>(mem);
    header->tag = static_cast<uint16_t>(tag);
    header->flags = 0;
    header->size = static_cast<uint32_t>((total_bytes + 7) & ~static_cast<size_t>(7));
    return header;
}

static bool is_valid_object_tag(uint16_t tag) noexcept {
    return (tag >= 0xFFF1 && tag <= 0xFFF8) || tag == static_cast<uint16_t>(Tag::Forwarded);
}

void Heap::forward_value(Value& val) {
    if (!val.isPointer()) {
        return;
    }

    void* payload_ptr = val.asObject<void>();
    if (!payload_ptr) {
        return;
    }

    auto* raw_ptr = static_cast<uint8_t*>(payload_ptr);
    if (raw_ptr < from_space_.base || raw_ptr >= from_space_.bump_ptr) {
        return;
    }

    auto* p1 = reinterpret_cast<HeapObjectHeader*>(raw_ptr);
    auto* p2 = p1 - 1;
    HeapObjectHeader* header = nullptr;

    if (reinterpret_cast<uint8_t*>(p2) >= from_space_.base && is_valid_object_tag(p2->tag)) {
        header = p2;
    } else if (is_valid_object_tag(p1->tag)) {
        header = p1;
    } else {
        return;
    }

    size_t offset = raw_ptr - reinterpret_cast<uint8_t*>(header);

    if (header->tag == static_cast<uint16_t>(Tag::Forwarded)) {
        auto* new_hdr = *reinterpret_cast<HeapObjectHeader**>(header->payload());
        uint8_t* updated_ptr = reinterpret_cast<uint8_t*>(new_hdr) + offset;
        val = Value::fromTagAndPayload(val.tag(), reinterpret_cast<uintptr_t>(updated_ptr));
        return;
    }

    size_t total_size = header->size;
    uint8_t* new_mem = static_cast<uint8_t*>(allocate_in_space(to_space_, total_size));
    std::memcpy(new_mem, header, total_size);
    auto* new_hdr = reinterpret_cast<HeapObjectHeader*>(new_mem);

    header->tag = static_cast<uint16_t>(Tag::Forwarded);
    *reinterpret_cast<HeapObjectHeader**>(header->payload()) = new_hdr;

    uint8_t* updated_ptr = reinterpret_cast<uint8_t*>(new_hdr) + offset;
    val = Value::fromTagAndPayload(val.tag(), reinterpret_cast<uintptr_t>(updated_ptr));
}

void Heap::collect() {
    if (in_gc_) {
        return;
    }

    in_gc_ = true;

    if (collection_hook_) {
        collection_hook_(*this);
    }

    to_space_.bump_ptr = to_space_.base;

    for (ShadowStackFrame* frame = ShadowStackFrame::current(); frame != nullptr; frame = frame->prev()) {
        Value** root_slots = frame->roots();
        size_t count = frame->count();
        for (size_t i = 0; i < count; ++i) {
            if (root_slots[i]) {
                forward_value(*root_slots[i]);
            }
        }
    }

    uint8_t* scan_ptr = to_space_.base;
    while (scan_ptr < to_space_.bump_ptr) {
        auto* scan_hdr = reinterpret_cast<HeapObjectHeader*>(scan_ptr);
        size_t obj_size = scan_hdr->size;

        if (scan_hdr->tag != static_cast<uint16_t>(Tag::String)) {
            uint8_t* payload_start = reinterpret_cast<uint8_t*>(scan_hdr->payload());
            size_t payload_bytes = obj_size - sizeof(HeapObjectHeader);
            size_t num_slots = payload_bytes / sizeof(Value);
            auto* slots = reinterpret_cast<Value*>(payload_start);
            for (size_t i = 0; i < num_slots; ++i) {
                forward_value(slots[i]);
            }
        }

        scan_ptr += obj_size;
    }

    from_space_.bump_ptr = from_space_.base;
    std::swap(from_space_, to_space_);

    in_gc_ = false;
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
