// bronze's heap over brass's collector (heap.h): the brass::gc::Heap a bronze
// Heap owns and how it is configured, bronze allocation onto it, the roots
// every heap has (the two shadow-stack chains), the write barrier's per-thread
// heap list, the binding of the thread's inline-allocation window to the
// young bump region, and the non-moving arena beside the heap. What the
// collector traces inside an object is heap_trace.cpp.

#include "runtime/heap.h"

#include "abi/bronze_abi.h"
#include "runtime/elem_ic.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap_trace.h"
#include "runtime/tls_block.h"

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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

namespace bronze {

namespace gc_detail {
thread_local BarrierHeaps t_barrierHeaps{};
}  // namespace gc_detail

void gcCopyValues(const void* object, HeapValue* dst, const Value* src, size_t count) noexcept {
    if (count == 0) return;
    std::memmove(static_cast<void*>(dst), src, count * sizeof(Value));
    gcRememberObject(object);
}

void gcFillValues(const void* object, HeapValue* dst, Value v, size_t count) noexcept {
    if (count == 0) return;
    auto* raw = reinterpret_cast<Value*>(dst);
    for (size_t i = 0; i < count; ++i) raw[i] = v;
    if (v.isPointer()) gcRememberObject(object);
}

namespace {

bool envIsOne(const char* name) {
    const char* v = std::getenv(name);
    return v && std::strcmp(v, "1") == 0;
}

bool envIsOn(const char* name) {
    const char* v = std::getenv(name);
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 || std::strcmp(v, "ON") == 0);
}

// Measurement, not policy: BRONZE_GC_LOG=1 prints at exit how much of a run
// the collector was on the first thread that enabled it — collections by kind,
// pause totals and maxima, and bytes allocated.
struct GcLog {
    bool enabled{false};
    const brass::gc::Heap* heap{nullptr};
    std::chrono::steady_clock::time_point start;
};
GcLog g_gcLog;

void dumpGcLog() {
    if (!g_gcLog.enabled || !g_gcLog.heap) return;
    const brass::gc::HeapStats& st = g_gcLog.heap->stats();
    const auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - g_gcLog.start)
                              .count();
    std::fprintf(stderr, "\n=== Bronze GC Log (BRONZE_GC_LOG=1) ===\n");
    std::fprintf(stderr, "minor collections: %llu (%.3f ms total, %.3f ms max)\n",
                 static_cast<unsigned long long>(st.minor_collections), st.minor_pause_ns_total / 1e6,
                 st.minor_pause_ns_max / 1e6);
    std::fprintf(stderr, "full collections : %llu (%.3f ms total, %.3f ms max)\n",
                 static_cast<unsigned long long>(st.full_collections), st.full_pause_ns_total / 1e6,
                 st.full_pause_ns_max / 1e6);
    std::fprintf(stderr, "allocated        : %.2f MB\n",
                 static_cast<double>(g_gcLog.heap->allocated_bytes()) / (1024.0 * 1024.0));
    std::fprintf(stderr, "promoted         : %.2f MB\n",
                 static_cast<double>(st.promoted_bytes) / (1024.0 * 1024.0));
    std::fprintf(stderr, "process wall     : %.3f ms\n", static_cast<double>(total_ns) / 1e6);
    std::fflush(stderr);
}

// Every reference a bronze heap holds is a NaN-boxed Value, so the pointer
// tags are the reference tags and a raw word (tag 0) is never one: a host
// pointer, a shape pointer or a small integer in a slot or an interpreter
// register is left alone.
brass::gc::HeapConfig bronzeHeapConfig() {
    brass::gc::HeapConfig config;
    config.reference_tags = {static_cast<uint16_t>(Tag::Object), static_cast<uint16_t>(Tag::String),
                             static_cast<uint16_t>(Tag::Symbol), static_cast<uint16_t>(Tag::BigInt)};
    config.read_environment = true;
    return config;
}

}  // namespace

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
        throw std::runtime_error("VirtualAlloc reserved address exceeds 47-bit range");
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
        throw std::runtime_error("mmap reserved address exceeds 47-bit range");
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

Heap::Heap() : gc_(std::make_unique<brass::gc::Heap>(bronzeHeapConfig())) {
    gc_detail::BarrierHeaps& barrier = gc_detail::t_barrierHeaps;
    if (barrier.count >= gc_detail::BarrierHeaps::kMax) {
        fatal("bronze: too many heaps on one thread for the write barrier's heap list");
    }
    barrier.heaps[barrier.count++] = gc_.get();

    // First, so every later hook can ask which kind of collection runs it.
    gc_->add_post_collection_hook([this](brass::gc::Heap&, brass::gc::CollectionKind kind) {
        last_full_ = kind == brass::gc::CollectionKind::Full;
    });
    registerFrameRoots();

    if (envIsOn("BRONZE_GC_STRESS")) set_gc_stress(true);
    if (envIsOne("BRONZE_GC_POISON")) set_gc_poison(true);
    if (envIsOne("BRONZE_HEAP_VERIFY")) set_gc_verify(true);

    runtime::rtReadThreadSeams();

    // The ident sweep runs inside every collection of THIS heap: an address is
    // reused only across a collection, so clearing every heap ident before the
    // mutator resumes is what makes the inline string arm's single-compare
    // guard sound (elem_ic.h). The bounds are the whole reservation.
    {
        const uintptr_t ident_lo = gc_->reservation_base();
        const uintptr_t ident_hi = ident_lo + gc_->reservation_bytes();
        add_post_collection_hook(
            [ident_lo, ident_hi] { runtime::elemCacheSweepIdent(ident_lo, ident_hi); });
        // And a full wipe now: the thread's table may carry idents from an
        // earlier Heap (unit tests construct them directly), and a fresh
        // reservation can land where an old one was.
        runtime::elemCacheSweepIdent(0, UINTPTR_MAX);
    }

    if (envIsOne("BRONZE_GC_LOG") && !g_gcLog.enabled) {
        g_gcLog.enabled = true;
        g_gcLog.heap = gc_.get();
        g_gcLog.start = std::chrono::steady_clock::now();
        std::atexit(dumpGcLog);
    }
}

Heap::~Heap() {
    if (bound_) {
        // The window is this heap's young bump region, released below.
        gc_->bind_allocation_buffer(nullptr);
        bronze_tls_block* tls = runtime::rtTls();
        tls->alloc_cursor = 0;
        tls->alloc_limit = 0;
        if (brass::gc::Heap::current() == gc_.get()) brass::gc::Heap::set_current(nullptr);
    }
    if (g_gcLog.heap == gc_.get()) g_gcLog.enabled = false;
    gc_detail::BarrierHeaps& barrier = gc_detail::t_barrierHeaps;
    for (uint32_t i = 0; i < barrier.count; ++i) {
        if (barrier.heaps[i] != gc_.get()) continue;
        for (uint32_t j = i + 1; j < barrier.count; ++j) barrier.heaps[j - 1] = barrier.heaps[j];
        --barrier.count;
        break;
    }
}

void Heap::registerFrameRoots() {
    gc_->add_root_source([](brass::gc::Tracer& t) {
        // The C++ side's shadow stack: Rooted<>, RootedArgs, RootedBlock.
        for (ShadowStackFrame* frame = ShadowStackFrame::current(); frame != nullptr; frame = frame->prev()) {
            Value** slots = frame->roots();
            const size_t count = frame->count();
            for (size_t i = 0; i < count; ++i) {
                if (slots[i]) t.visit(reinterpret_cast<uint64_t*>(slots[i]));
            }
        }
        // Generated code's root frames: contiguous slot arrays in compiled
        // functions' own stack frames, linked inline by compiled code.
        for (bronze_gc_frame* frame = runtime::rtTls()->frame_top; frame != nullptr; frame = frame->prev) {
            for (uint64_t i = 0; i < frame->count; ++i) t.visit(&frame->slots[i]);
        }
    });
}

HeapObjectHeader* Heap::allocate(size_t bytes, Tag tag, GcLayout layout) {
    const size_t total = (sizeof(HeapObjectHeader) + bytes + 7) & ~static_cast<size_t>(7);
    if (total > UINT32_MAX) throw std::bad_alloc();
    const gc_detail::LayoutIds& ids = gc_detail::layoutIds();
    brass::gc::LayoutId id = ids.cell;
    switch (layout) {
        case GcLayout::Auto:
            id = (tag == Tag::String || tag == Tag::BigInt) ? ids.leaf : ids.cell;
            break;
        case GcLayout::Cell: id = ids.cell; break;
        case GcLayout::Leaf: id = ids.leaf; break;
        case GcLayout::WeakLast: id = ids.weakLast; break;
        case GcLayout::Ephemerons: id = ids.ephemerons; break;
    }
    auto* header = reinterpret_cast<HeapObjectHeader*>(gc_->allocate(total, id));
    header->tag = static_cast<uint16_t>(tag);
    // A raw zero and deliberately not `HeapKind::Plain`: this word is a heap
    // kind only for a `Tag::Object`, and a String spends it on its encoding
    // bits. The caller that knows which tag it asked for names what goes here.
    header->flags = 0;
    header->size = static_cast<uint32_t>(total);
    return header;
}

void Heap::collect() {
    if (gc_->in_collection()) return;
    gc_->collect(brass::gc::CollectionKind::Full);
}

void Heap::collect_minor() {
    if (gc_->in_collection()) return;
    gc_->collect(brass::gc::CollectionKind::Minor);
}

void Heap::add_post_collection_hook(PostCollectionHook hook) {
    gc_->add_post_collection_hook(
        [hook = std::move(hook)](brass::gc::Heap&, brass::gc::CollectionKind) { hook(); });
}

HeapObjectHeader* Heap::survivor_of(HeapObjectHeader* header) const noexcept {
    return reinterpret_cast<HeapObjectHeader*>(gc_->survivor_of(reinterpret_cast<uintptr_t>(header)));
}

void Heap::add_permanent_root(Value* slot) { gc_->add_root(reinterpret_cast<uint64_t*>(slot)); }

void Heap::add_root_source(RootSource src) {
    gc_->add_root_source([src = std::move(src)](brass::gc::Tracer& t) {
        src([&t](Value& slot) { t.visit(reinterpret_cast<uint64_t*>(&slot)); });
    });
}

void Heap::add_tracer_source(std::function<void(brass::gc::Tracer&)> src) {
    gc_->add_root_source(std::move(src));
}

void Heap::set_gc_stress(bool enable) noexcept {
    gc_->set_stress(enable ? brass::gc::StressMode::Alternate : brass::gc::StressMode::None);
}

bool Heap::gc_stress() const noexcept { return gc_->stress() != brass::gc::StressMode::None; }

void Heap::set_gc_poison(bool enable) noexcept {
    poison_ = enable;
    gc_->set_poison(enable);
}

void Heap::set_gc_verify(bool enable) noexcept {
    verify_ = enable;
    gc_->set_verify(enable);
}

void Heap::walk_objects(const std::function<void(HeapObjectHeader*)>& fn) {
    const brass::gc::Heap& heap = *gc_;
    heap.for_each_object([&](uintptr_t object) {
        if (!gc_detail::isBronzeLayout(brass::gc::header_of(object)->layout)) return;
        fn(reinterpret_cast<HeapObjectHeader*>(object));
    });
}

void Heap::bind_thread() {
    brass::gc::Heap::set_current(gc_.get());
    bronze_tls_block* tls = runtime::rtTls();
    // brass's object header for an inline allocation, size left zero
    // (bronze_abi_tls.h, gc_cell_header).
    tls->gc_cell_header = static_cast<uint64_t>(gc_detail::layoutIds().cell) << 32;
    // BRONZE_NO_INLINE_ALLOC=1: the window stays 0/0, no size fits, and every
    // allocation takes its helper — the A/B seam for the inline path.
    if (!envIsOne("BRONZE_NO_INLINE_ALLOC")) {
        gc_->bind_allocation_buffer(reinterpret_cast<brass::gc::Heap::AllocationBuffer*>(&tls->alloc_cursor));
    }
    bound_ = true;
}

NonMovingArena::NonMovingArena(size_t chunk_size) : chunk_size_(chunk_size) {}

NonMovingArena::~NonMovingArena() {
    for (auto& d : destructors_) {
        d.dtor(d.ptr);
    }
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
