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

    struct Semispace {
        uint8_t* base{nullptr};
        size_t size{0};
        size_t committed_bytes{0};
        uint8_t* bump_ptr{nullptr};
    };

    explicit Heap(size_t reserve_bytes = 1024 * 1024 * 1024, size_t initial_commit_bytes = 64 * 1024);
    ~Heap();

    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    HeapObjectHeader* allocate(size_t bytes, Tag tag);
    void* allocate_raw(size_t bytes);

    void set_collection_hook(CollectionHook hook) { collection_hook_ = std::move(hook); }
    void collect();

    // A root that outlives every frame: runtime-owned caches of heap
    // objects (lazily created builtins, and later the global object). The
    // slot must outlive the heap; registering the same slot twice is a
    // caller error, not something this checks for.
    void add_permanent_root(Value* slot) { permanent_roots_.push_back(slot); }

    // A root *source*: a callback invoked at collection time that yields
    // every slot in a runtime-owned table. add_permanent_root pins one
    // fixed address, which cannot describe a table that grows (and so
    // reallocates) during the run — the shape registry's prototype slots
    // (docs/0008) are the first such table.
    using RootVisitor = std::function<void(Value&)>;
    using RootSource = std::function<void(const RootVisitor&)>;
    void add_root_source(RootSource src) { root_sources_.push_back(std::move(src)); }

    void set_gc_stress(bool enable) noexcept { gc_stress_mode_ = enable; }
    bool gc_stress() const noexcept { return gc_stress_mode_; }

    // How many objects this collector has RELOCATED. A hash table keyed on
    // VALUES rather than on property names (a Map, docs/0021 decision 4)
    // hashes an object key by its address, so every such table records this
    // number when it builds its index and rebuilds when the number has moved
    // on. Nothing else can tell it that every object-key hash it holds is now
    // wrong.
    //
    // It counts RELOCATIONS and not collections, and the difference is the
    // whole point (docs/0022): a "collections completed" counter lives at the
    // end of `collect()`, so a second collection entry point — a nursery
    // sweep, a compaction, anything that moves objects without finishing a
    // full cycle — would move objects while leaving the count alone, and every
    // Map index in the program would silently answer "not found" for a live
    // key. This counter is incremented by the copy itself, so the only way to
    // move an object past it is to write a second object-copy routine.
    uint64_t relocation_epoch() const noexcept { return relocations_; }

    // How many collections have completed. Statistics; nothing about
    // correctness may hang off it — see relocation_epoch above.
    uint64_t collection_count() const noexcept { return collections_; }

    uintptr_t base_address() const noexcept { return reinterpret_cast<uintptr_t>(from_space_.base); }
    size_t reserved_size() const noexcept { return reserved_bytes_; }
    size_t committed_size() const noexcept { return from_space_.committed_bytes; }
    size_t used_size() const noexcept {
        return from_space_.bump_ptr - from_space_.base;
    }

    const Semispace& from_space() const noexcept { return from_space_; }
    const Semispace& to_space() const noexcept { return to_space_; }

private:
    bool ensure_commit(Semispace& space, size_t required_bytes);
    void* allocate_in_space(Semispace& space, size_t bytes);
    void forward_value(Value& val);

    void* reserved_base_{nullptr};
    size_t reserved_bytes_{0};
    size_t semispace_size_{0};

    Semispace from_space_;
    Semispace to_space_;

    std::vector<Value*> permanent_roots_;
    std::vector<RootSource> root_sources_;
    bool gc_stress_mode_{false};
    bool in_gc_{false};
    uint64_t collections_{0};
    uint64_t relocations_{0};
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
