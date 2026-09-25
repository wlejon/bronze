#pragma once

#include <brass/gc/heap.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "runtime/value.h"

namespace bronze {

// Every bronze heap object starts with this word. The object lives on
// brass's collector (brass::gc::Heap): its bytes, header included, are the
// PAYLOAD of one brass object, so a Value's address — which names this header —
// is the brass reference to it. `size` is the bronze object's total size,
// header included, 8-byte aligned; the brass object is never smaller.
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

// What kind of thing a `Tag::Object` allocation is. The `flags` word above is
// the only way to tell, because every one of these is reached through the same
// `Value` representation and the same `asObject<T>()` cast — so a wrong answer
// here is not a wrong answer, it is reading one type's memory as another's.
//
// They are enumerated in ONE place so that no two of them can share a number:
// an environment record and a Map once both answered 5, and `resolveEnv`'s
// brand check accepted a Map and walked its payload as scope slots.
//
// The numbers are NOT free to change: brass's allocation lowering writes a
// kind into a fresh header as a literal (il_alloc_lowering.cpp — Plain, Array,
// Env and ValueBlock), so each enumerator here carries its value explicitly
// and a retired kind leaves a hole rather than shifting its neighbours.
//
// Holes 6-9 and 15-17 belonged to Map, Set, WeakMap, WeakSet, PrivateTable,
// WeakRef and FinalizationRegistry. Each is a PLAIN object — a real prototype
// chain, its state in internal slots, told apart by a brand symbol in slot 0
// exactly as a Date is (runtime/map.h, runtime/weak_ref.h) — so no dispatch in
// the runtime needs a kind for one.
namespace HeapKind {
enum : uint16_t {
    Plain = 0,  // an ordinary object; ties to BRONZE_ABI_OBJ_FLAGS_PLAIN in object.h
    Array = 1,
    Function = 2,
    TypedArray = 3,
    ArrayBuffer = 4,
    // A DataView is its own kind and not a tenth element kind: it reads a
    // buffer at an arbitrary byte offset with an explicitly named byte order,
    // so nothing about it shares the offset-times-width addressing every
    // %TypedArray% element access is.
    DataView = 5,
    Iterator = 10,
    RegExp = 11,
    Env = 12,
    // A module namespace exotic object (ECMA-262 10.4.6). Its own kind because
    // three of its internal methods are not the ordinary ones — sorted own
    // keys, a [[Set]] that always refuses, a non-configurable descriptor — and
    // none of the three is expressible as an attribute on a plain object's
    // property. See runtime/namespace.h.
    ModuleNamespace = 13,
    // A Proxy exotic object (10.5), carrying its target and handler. Its own
    // kind for the same reason a namespace is: [[Get]], [[Set]] and
    // [[HasProperty]] are not the ordinary internal methods, and no attribute
    // on a plain object's property can express "ask the handler first".
    // runtime/proxy.h owns the layout and the construction gate that keeps
    // every OTHER internal method forwardable.
    Proxy = 14,

    // An object's OUT-OF-LINE PROPERTY SLOTS — the block `ObjectHeader::
    // overflow` names, holding slot `kInlineSlots` and up. Not a JS value and
    // not reachable as one: exactly one word in the program points at it, and
    // that word is a field of the object that owns it. Every word of it is a
    // Value (a double slot holds a canonical Number, which is never a
    // reference), so the collector traces it like a ValueBlock; it has a kind
    // of its own so that it never reads as `HeapKind::Plain`, which is a claim
    // that a `Shape*` sits at offset 8.
    SlotBlock = 18,

    // A flat run of Values that is not an object at all and has no header
    // fields of its own: an array's ELEMENTS, a Map's entry table. Every word
    // of one is a Value.
    ValueBlock = 19,

    // Not a kind: one past the highest number in use. It exists so that a
    // dispatch which must be TOTAL over the registry can pin the registry's
    // size and break the build when a kind is added. `flags` is a `uint16_t` and this enum is
    // unnamed, so no compiler warning can check such a switch for
    // exhaustiveness — a static_assert on this number is the only tripwire
    // available, and `bronze_has_property` is why one is needed: a kind with no
    // arm there once fell through to a cast that read its payload's first word
    // as a `Shape*`.
    Count = 20,
};

// Does a header of this kind BEGIN with an `ObjectHeader` — a shape word, an
// overflow word and the inline property slots — whatever it carries after
// them? A plain object is the one kind with nothing after; a typed array, an
// ArrayBuffer and a DataView (typed_array.h) and a RegExp (regexp.h) carry
// their state behind the prefix, exactly so that the ordinary property
// machinery — the shape walk, the inline caches, expandos, `Object.keys`,
// [[Prototype]] — reads them as objects while the kind still names them for
// the element paths and the matcher.
//
// The two questions are different and every gate must ask the one it means:
// "may I read this as an ObjectHeader" is this; "is this an ordinary object
// and nothing more" keeps comparing against `Plain`.
inline constexpr bool carriesShape(uint16_t flags) noexcept {
    return flags == Plain || flags == TypedArray || flags == ArrayBuffer || flags == DataView ||
           flags == RegExp;
}
}  // namespace HeapKind

// How the collector finds an object's references (heap_trace.cpp). Chosen
// once, at allocation.
enum class GcLayout : uint8_t {
    // By the header's tag: a String or a BigInt holds none, anything else is
    // a Cell.
    Auto,
    // A bronze object or block traced by its header: every Value word of it
    // (for a kind that carries a shape, the words after the shape pointer; for
    // an ArrayBuffer, its ordinary-object prefix only; for other RawBytes,
    // nothing).
    Cell,
    // No references at all.
    Leaf,
    // A Cell whose LAST word is a weak reference (a WeakRef's target): the
    // collector does not keep its target alive and writes `undefined` there
    // when the target dies.
    WeakLast,
    // A ValueBlock of (key, value) pairs whose values are held only as long as
    // their keys are otherwise alive (a WeakMap's or WeakSet's entry table):
    // a dead key's pair becomes the table's tombstone, (Hole, undefined).
    Ephemerons,
};

namespace gc_detail {

// The heaps on this thread, for the write barrier: a store must remember the
// slot on whichever heap holds it. The runtime's own heap is one of them;
// a test may construct more. Registered by Heap's constructor.
struct BarrierHeaps {
    static constexpr uint32_t kMax = 8;
    brass::gc::Heap* heaps[kMax];
    uint32_t count;
};
extern thread_local BarrierHeaps t_barrierHeaps;

}  // namespace gc_detail

// The write barrier: every store of a Value into a heap object that may be
// old goes through this (or through HeapValue, which calls it), so a minor
// collection finds the old objects that name young ones. `slot` is the
// address stored to, anywhere inside the object.
inline void gcWriteBarrier(const void* slot, Value v) noexcept {
    if (!v.isPointer()) return;
    const gc_detail::BarrierHeaps& b = gc_detail::t_barrierHeaps;
    for (uint32_t i = 0; i < b.count; ++i) {
        b.heaps[i]->write_barrier_interior(reinterpret_cast<uintptr_t>(slot), v.rawBits());
    }
}

// The barrier for a bulk copy (memcpy/memmove of many Values) into `object`,
// whose header this must be: an old object is rescanned whole by the next
// minor collection.
inline void gcRememberObject(const void* object) noexcept {
    const gc_detail::BarrierHeaps& b = gc_detail::t_barrierHeaps;
    for (uint32_t i = 0; i < b.count; ++i) {
        b.heaps[i]->remember(reinterpret_cast<uintptr_t>(object));
    }
}

// A Value that lives inside a heap object: assigning to it applies the write
// barrier. Every Value field of a heap struct, and every accessor that hands
// out a heap object's Value array, uses this type, so an ordinary assignment
// is a barriered store. It reads as the Value it holds, and converts to one.
//
// It is not a Value subclass on purpose: a `HeapValue*` does not convert to a
// `Value*`, so no pointer into heap memory can be written through without the
// barrier by accident. Code that hands a run of them to something that only
// reads takes `values()`; code that writes a run in bulk uses gcCopyValues or
// calls gcRememberObject after it.
class HeapValue {
public:
    HeapValue() noexcept = default;
    // Explicit, so a HeapValue meeting a Value (`c ? field : Value(...)`)
    // decays to the Value rather than the other way round.
    constexpr explicit HeapValue(Value v) noexcept : v_(v) {}
    HeapValue(const HeapValue&) noexcept = default;
    HeapValue& operator=(Value v) noexcept {
        v_ = v;
        gcWriteBarrier(this, v);
        return *this;
    }
    HeapValue& operator=(const HeapValue& v) noexcept { return *this = v.v_; }

    constexpr operator Value() const noexcept { return v_; }
    constexpr Value get() const noexcept { return v_; }

    constexpr uint64_t rawBits() const noexcept { return v_.rawBits(); }
    constexpr uint16_t tag() const noexcept { return v_.tag(); }
    constexpr uint64_t payload() const noexcept { return v_.payload(); }
    constexpr bool isNumber() const noexcept { return v_.isNumber(); }
    double asNumber() const noexcept { return v_.asNumber(); }
    constexpr bool isBool() const noexcept { return v_.isBool(); }
    constexpr bool asBool() const noexcept { return v_.asBool(); }
    constexpr bool isNull() const noexcept { return v_.isNull(); }
    constexpr bool isUndefined() const noexcept { return v_.isUndefined(); }
    constexpr bool isHole() const noexcept { return v_.isHole(); }
    constexpr bool isUninitialized() const noexcept { return v_.isUninitialized(); }
    constexpr bool isObject() const noexcept { return v_.isObject(); }
    constexpr bool isString() const noexcept { return v_.isString(); }
    constexpr bool isSymbol() const noexcept { return v_.isSymbol(); }
    constexpr bool isInt32() const noexcept { return v_.isInt32(); }
    constexpr bool isBigInt() const noexcept { return v_.isBigInt(); }
    constexpr bool isPointer() const noexcept { return v_.isPointer(); }
    template <typename T = void>
    T* asObject() const noexcept { return v_.asObject<T>(); }
    template <typename T = void>
    T* asString() const noexcept { return v_.asString<T>(); }
    template <typename T = void>
    T* asSymbol() const noexcept { return v_.asSymbol<T>(); }
    template <typename T = void>
    T* asBigInt() const noexcept { return v_.asBigInt<T>(); }

    constexpr bool operator==(const Value& other) const noexcept { return v_ == other; }

    // A run of HeapValues read as Values, for code that only reads them.
    static const Value* values(const HeapValue* p) noexcept { return reinterpret_cast<const Value*>(p); }

private:
    Value v_;
};
static_assert(sizeof(HeapValue) == sizeof(Value), "a HeapValue is a Value in place");
static_assert(std::is_standard_layout_v<HeapValue>);
static_assert(std::is_trivially_destructible_v<HeapValue>);

// Copies `count` Values into heap memory at `dst` (overlap allowed), inside the
// object whose header is `object`, with the barrier a bulk copy needs.
void gcCopyValues(const void* object, HeapValue* dst, const Value* src, size_t count) noexcept;
inline void gcCopyValues(const void* object, HeapValue* dst, const HeapValue* src, size_t count) noexcept {
    gcCopyValues(object, dst, HeapValue::values(src), count);
}
// Stores `v` into `count` Values at `dst`, inside the object whose header is
// `object`.
void gcFillValues(const void* object, HeapValue* dst, Value v, size_t count) noexcept;

class VirtualMemory {
public:
    static void* reserve(size_t bytes);
    static bool commit(void* ptr, size_t bytes);
    static void decommit(void* ptr, size_t bytes);
    static void release(void* ptr, size_t bytes);
};

// bronze's view of its collector: one brass::gc::Heap per thread
// (runtime/rt_state.cpp, rtHeap), generational — a copying young generation
// over a non-moving mark-region old generation and a large-object space —
// with bronze's object layouts, roots, weak tables and write barrier
// registered on it. brass/docs/gc_contract.md is the collector's contract;
// what bronze adds is here.
//
// A collection may happen at any allocation. A young object that survives one
// MOVES (every Value naming it is updated through its root or its referrer);
// an old object never moves. Nothing may rely on which of the two an object
// is, except through `relocation_epoch` and `is_movable`.
class Heap {
public:
    Heap();
    ~Heap();

    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    // A zeroed object of `bytes` payload bytes after its header, the header's
    // tag set to `tag`, flags 0 and size the total. May collect first.
    HeapObjectHeader* allocate(size_t bytes, Tag tag, GcLayout layout = GcLayout::Auto);

    // A full collection: every unreachable object is reclaimed.
    void collect();
    // A young-generation collection.
    void collect_minor();

    // Runs inside every collection once liveness is decided and weak slots
    // are settled — the one window in which `survivor_of` answers. The hook
    // must not allocate on this heap. Hooks run in registration order.
    using PostCollectionHook = std::function<void()>;
    void add_post_collection_hook(PostCollectionHook hook);

    // Where the object whose header was at `header` before this collection
    // lives now, or null when it died. Meaningful ONLY inside a
    // post-collection hook. An address this collection did not collect
    // (outside the heap, or old during a minor collection) is returned
    // unchanged.
    HeapObjectHeader* survivor_of(HeapObjectHeader* header) const noexcept;

    // Whether the running (or last) collection is a full one. Inside a
    // post-collection hook: the kind of the collection that runs it.
    bool collecting_full() const noexcept { return last_full_; }

    // A root that outlives every frame: runtime-owned caches of heap objects.
    // The slot must outlive the heap.
    void add_permanent_root(Value* slot);

    // A root SOURCE: a callback invoked at every collection that visits every
    // slot in a runtime-owned table, as a strong root.
    using RootVisitor = std::function<void(Value&)>;
    using RootSource = std::function<void(const RootVisitor&)>;
    void add_root_source(RootSource src);

    // A root source with the collector's whole vocabulary: strong, weak and
    // ephemeron visits (brass/gc/tracer.hpp).
    void add_tracer_source(std::function<void(brass::gc::Tracer&)> src);

    // BRONZE_GC_STRESS=1: a collection at every allocation — a minor one each
    // time and a full one every eighth — so a Value held across an allocation
    // without a root is caught at the first allocation after it.
    void set_gc_stress(bool enable) noexcept;
    bool gc_stress() const noexcept;

    // BRONZE_GC_POISON=1: memory a collection frees or evacuates is
    // overwritten with 0xDB, so the first stale read yields an impossible
    // value on every platform, every run.
    void set_gc_poison(bool enable) noexcept;
    bool gc_poison() const noexcept { return poison_; }

    // BRONZE_HEAP_VERIFY=1: the collector checks every object and root before
    // and after each collection — every reference names an object, and every
    // old object naming a young one is remembered (a missing write barrier)
    // — and stops the process naming the object and slot.
    void set_gc_verify(bool enable) noexcept;
    bool gc_verify() const noexcept { return verify_; }

    // Visit every object the heap holds, live or not yet reclaimed. The
    // callback must not allocate on this heap, and must not follow a Value out
    // of an object it has not established is live.
    void walk_objects(const std::function<void(HeapObjectHeader*)>& fn);

    // Advances every time a collection MOVES an object. A hash table that
    // hashes an object key by its address records this number when it builds
    // its index and rebuilds when it has moved on — unless every such key is
    // old (`is_movable` false), since an old object never moves.
    uint64_t relocation_epoch() const noexcept { return gc_->relocation_epoch(); }
    // Whether the object at `header` may move in a later collection.
    bool is_movable(const void* header) const noexcept {
        return gc_->is_young(reinterpret_cast<uintptr_t>(header));
    }
    bool contains(const void* address) const noexcept {
        return gc_->contains(reinterpret_cast<uintptr_t>(address));
    }

    // How many collections have completed. Statistics only.
    uint64_t collection_count() const noexcept { return gc_->collection_count(); }
    uint64_t last_pause_ns() const noexcept { return gc_->stats().last_pause_ns; }

    size_t reserved_size() const noexcept { return gc_->reservation_bytes(); }
    size_t committed_size() const noexcept { return gc_->committed_bytes(); }
    size_t used_size() const noexcept { return gc_->young_used_bytes() + gc_->old_used_bytes(); }

    // The largest object this heap allocates; a request above it is a
    // RangeError the caller reports rather than an allocation.
    static constexpr size_t kMaxObjectBytes = size_t{512} << 20;

    brass::gc::Heap& gc() noexcept { return *gc_; }
    const brass::gc::Heap& gc() const noexcept { return *gc_; }

    // Makes this the calling thread's heap for generated code and brass's
    // runtime: brass's current heap (its interpreters' roots and memory checks),
    // and — unless BRONZE_NO_INLINE_ALLOC=1 — the inline-allocation window in
    // the thread's bronze_tls_block, which becomes this heap's young bump
    // region. rtHeap() calls it once per thread.
    void bind_thread();

private:
    void registerFrameRoots();

    std::unique_ptr<brass::gc::Heap> gc_;
    bool poison_{false};
    bool verify_{false};
    bool bound_{false};
    bool last_full_{false};
};

class NonMovingArena {
public:
    explicit NonMovingArena(size_t chunk_size = 64 * 1024);
    ~NonMovingArena();

    NonMovingArena(const NonMovingArena&) = delete;
    NonMovingArena& operator=(const NonMovingArena&) = delete;

    void* allocate(size_t bytes, size_t alignment = 8);

    struct DestructorEntry {
        void* ptr;
        void (*dtor)(void*);
    };

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        T* obj = new (mem) T(std::forward<Args>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            destructors_.push_back({obj, [](void* p) { static_cast<T*>(p)->~T(); }});
        }
        return obj;
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
    std::vector<DestructorEntry> destructors_;
};

}  // namespace bronze
