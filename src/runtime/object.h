#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze {

// A monomorphic property cache: three plain words, none of which the
// collector has to touch. `cached_depth` is how many prototype links to
// follow from the receiver before reading `cached_slot` — 0 for an own
// property, so this one form covers own hits and proto hits alike. The
// holder is derived from the (non-moving) shape chain rather than cached,
// which is what keeps the entry GC-free; see docs/0008 decision 2.
//
// An entry ALWAYS describes a data property in a shape-indexed slot, because
// that is the only thing its consumers can do with one — generated code
// inlines a load (docs/0010 decision 7) and cannot call a getter, and a
// dictionary's slots are not shape-indexed. Accessors and dictionary
// receivers are therefore never written here; docs/0019 decision 5 is the
// full argument, including why a cached proto hit must re-check the holder.
struct InlineCache {
    Shape* cached_shape{nullptr};
    uint32_t cached_slot{0};
    uint32_t cached_depth{0};
};

struct ObjectHeader {
    HeapObjectHeader header;
    Shape* shape{nullptr};
    // Undefined, or an Object-tagged pointer to the HEADER of a heap block
    // of Values holding slots kInlineSlots and up. Stored as a Value so the
    // generic GC payload scan forwards it like any other slot; header, not
    // payload, because every heap reference in a Value points at a header.
    Value overflow;

    static constexpr uint32_t kInlineSlots = 4;

    // A cycle in a prototype chain would hang the property path rather than
    // crash it, so every walk over it is bounded and says so by name. Real
    // chains are 1–3 links.
    static constexpr uint32_t kMaxPrototypeDepth = 1000;

    // `shape` is required: it decides the object's prototype (docs/0008
    // decision 1), and minting a fresh root shape per object would give
    // every `{}` literal an unrelated hidden class.
    static ObjectHeader* create(Heap& heap, NonMovingArena& arena, Shape* shape);

    // The object `cached_depth` prototype links up from this one, or null
    // if the chain is shorter than that. No allocation, so the raw pointer
    // is safe to the next allocation.
    ObjectHeader* protoAncestor(uint32_t depth) noexcept;

    // May allocate and may run USER CODE: a property whose own-or-inherited
    // definition is an accessor calls its getter here (docs/0019 decision 3).
    // So `this` must be reachable from a root at the call and must not be
    // reused afterwards — the same contract setProp has always had.
    //
    // `receiver` is what `this` is bound to if the property turns out to be
    // an accessor: the object itself for every ordinary read, which is why
    // it defaults to null. A function's STATIC members live in a side object
    // (docs/0012 decision 6), so that one caller passes the constructor down
    // and a static getter sees the class rather than the box its properties
    // are kept in.
    Value getProp(Heap& heap, Rooted<Value>& key, InlineCache* ic = nullptr,
                  const Value* receiver = nullptr);
    // May allocate (overflow growth), which can move this object; use the
    // returned pointer afterwards, not `this`. May also run user code, for
    // the same reason getProp can: an inherited setter.
    //
    // `enumerable` decides the ATTRIBUTE a newly created property gets, and
    // is therefore part of the shape transition it takes. It is false for
    // exactly one caller — a class method definition (docs/0018 decision 2) —
    // and an ordinary assignment never reaches it, because assignment always
    // creates an enumerable property.
    //
    // `defineOwn` switches from Set (ECMA-262 10.1.9) to DefineOwnProperty
    // (10.1.6): a definition never runs an inherited setter. A class method
    // and an object spread are definitions; `o.k = v` is not, and the
    // difference is only observable now that a prototype can carry one.
    ObjectHeader* setProp(Heap& heap, NonMovingArena& arena, Rooted<Value>& key,
                          Rooted<Value>& val, InlineCache* ic = nullptr,
                          bool enumerable = true, bool defineOwn = false,
                          const Value* receiver = nullptr);

    // `delete o.k`, which removes an OWN property and answers true — also
    // when the property was never there, and when only a prototype has it
    // (ECMA-262 13.5.1 / 10.5.6). Removing from the middle of a transition
    // chain is impossible, so the first successful delete moves the object
    // to dictionary mode (docs/0019 decision 1). Allocates nothing on the
    // heap; the dictionary and its shape live in the arena.
    bool deleteProperty(NonMovingArena& arena, StringHeader* name);

    // `get k() {}` / `set k(v) {}`. Defines ONE property with two halves: a
    // second call for the other half of the same name updates the pair
    // rather than creating a second property. Allocates (a shape transition
    // may grow the overflow block), so it takes the object through a root
    // and the caller must re-derive any raw pointer afterwards.
    static void defineAccessor(Heap& heap, NonMovingArena& arena, Rooted<Value>& self,
                               Rooted<Value>& key, Rooted<Value>& getter, Rooted<Value>& setter,
                               bool enumerable);

    // The object's own properties as a table that can be removed from the
    // middle of, and a private shape naming it. Idempotent.
    static void toDictionary(NonMovingArena& arena, Rooted<Value>& self);

    // Define an own property on a DICTIONARY-mode object, returning the live
    // object and, through `out_slot`, where the property's value goes. A name
    // already present keeps its POSITION in the enumeration order even when
    // its kind changes, which is what DefineOwnProperty says and what makes
    // dictionary mode the general answer for a redefinition the transition
    // tree cannot express.
    static ObjectHeader* dictDefine(Heap& heap, NonMovingArena& arena, Rooted<Value>& self,
                                    StringHeader* name, bool enumerable, bool accessor,
                                    uint32_t& out_slot);

    // Grow the out-of-line block so that `needed` overflow slots — or, for
    // ensureSlots, slot indices [0, count) — are addressable. Allocates, so
    // the object is reached through the root and `this` must not be reused.
    static ObjectHeader* ensureOverflow(Heap& heap, Rooted<Value>& self, uint32_t needed);
    static ObjectHeader* ensureSlots(Heap& heap, Rooted<Value>& self, uint32_t count);

    Value getSlot(uint32_t index) const;
    void setSlot(uint32_t index, Value val);

    uint32_t overflowCapacity() const noexcept {
        if (!overflow.isPointer()) return 0;
        const auto* hdr = overflow.asObject<HeapObjectHeader>();
        return static_cast<uint32_t>((hdr->size - sizeof(HeapObjectHeader)) / sizeof(Value));
    }

    Value* slotsData() noexcept {
        return reinterpret_cast<Value*>(this + 1);
    }
    const Value* slotsData() const noexcept {
        return reinterpret_cast<const Value*>(this + 1);
    }
};

// These two layouts are part of the generated-code ABI (docs/0010 decision
// 7): compiled code loads the shape word and the cache entry itself rather
// than calling a helper to do it. The constants live in bronze_abi.h, which
// is pure C and cannot see a C++ class, so this is where the two sides are
// tied together — deliberately in the HEADER, so every translation unit
// that can see the structs also checks them. Adding or reordering a field
// breaks the build here instead of miscompiling every property read.
static_assert(sizeof(InlineCache) == BRONZE_ABI_IC_ENTRY_SIZE);
static_assert(alignof(InlineCache) <= 8);
static_assert(offsetof(InlineCache, cached_shape) == BRONZE_ABI_IC_SHAPE_OFFSET);
static_assert(offsetof(InlineCache, cached_slot) == BRONZE_ABI_IC_SLOT_OFFSET);
static_assert(offsetof(InlineCache, cached_depth) == BRONZE_ABI_IC_DEPTH_OFFSET);
static_assert(sizeof(InlineCache::cached_slot) == 4 && sizeof(InlineCache::cached_depth) == 4,
              "the fast path reads slot and depth as one u64; both halves must be 32 bits");

static_assert(offsetof(HeapObjectHeader, flags) == BRONZE_ABI_OBJ_FLAGS_OFFSET);
static_assert(offsetof(ObjectHeader, shape) == BRONZE_ABI_OBJ_SHAPE_OFFSET);
static_assert(sizeof(ObjectHeader) == BRONZE_ABI_OBJ_SLOTS_OFFSET,
              "inline slots start immediately after ObjectHeader");
static_assert(ObjectHeader::kInlineSlots == BRONZE_ABI_OBJ_INLINE_SLOTS);

}  // namespace bronze
