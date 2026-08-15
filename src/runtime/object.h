#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "abi/bronze_abi.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze {

// Why an ordinary [[Set]] answered false (ECMA-262 10.1.9.2). Every one of
// them is a silent no-op in sloppy code and a TypeError in strict code, and
// they are the ONLY three ways an assignment can fail without throwing on its
// own — so this enum is also the list of what `strict` changes about a write.
enum class SetRefusal {
    None,
    NoSetter,       // an accessor property with no `set` half (step 5.c)
    NotWritable,    // a non-writable data property (step 3 -> 10.1.6.3)
    NotExtensible,  // a new property on a non-extensible receiver (10.1.6.3 2.b)
};

// The number of property ADDS that have happened anywhere in the program. A
// depth > 0 inline-cache entry records it and re-checks it, because the
// receiver's shape — the only thing the entry compares — cannot see a property
// appear on an object BETWEEN the receiver and the holder, and such a property
// shadows what the entry points at.
//
// Counting every add rather than only the ones that land on a prototype is
// deliberate and measured: identifying a prototype needs a per-object bit, and
// the imprecise counter costs nothing on the workloads bronze has. It only ever
// causes a MISS, never a wrong answer.
uint64_t protoMutationEpoch() noexcept;
void bumpProtoMutationEpoch() noexcept;

// A monomorphic property cache: four plain words, none of which the collector
// has to touch. `cached_depth` is how many prototype links to follow from the
// receiver before reading `cached_slot` — 0 for an own property, so this one
// form covers own hits and proto hits alike. The holder is derived from the
// (non-moving) shape chain rather than cached, which is what keeps the entry
// GC-free.
//
// An entry ALWAYS describes a data property in a shape-indexed slot, because
// that is the only thing its consumers can do with one — generated code inlines
// a load and cannot call a getter, and a dictionary's slots are not
// shape-indexed. Accessors and dictionary receivers are therefore never written
// here. A cached proto hit must re-check the holder, because a delete reuses
// freed slots for unrelated names.
struct InlineCache {
    Shape* cached_shape{nullptr};
    uint32_t cached_slot{0};
    uint32_t cached_depth{0};
    // The epoch this entry was filled at. Consulted only when
    // `cached_depth > 0`; at depth 0 the receiver's own shape already
    // changes whenever an own property is added to it, and an
    // array-method sentinel entry leaves it 0 and never reads it (the
    // method table is immutable by construction — decorating
    // `Array.prototype` is a hard error, rt_prop_write.cpp).
    uint64_t cached_epoch{0};

    bool isArrayMethod() const noexcept {
        return reinterpret_cast<uintptr_t>(cached_shape) == BRONZE_ABI_IC_SHAPE_ARRAY_METHOD;
    }

    bool isRealShape() const noexcept {
        return cached_shape != nullptr && !isArrayMethod();
    }

    bool isAccessor() const noexcept {
        return (cached_depth & BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG) != 0;
    }

    uint32_t realDepth() const noexcept {
        return cached_depth & ~BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG;
    }

    // Is this entry still about the chain it was filled against? Both runtime
    // hit paths — `bronze_prop_get`'s and `ObjectHeader::getProp`'s — ask here
    // rather than restating the condition, because the last time this question
    // had two copies they answered differently. It deliberately does NOT cover
    // whether the walk to the holder is safe to take; `cachedProtoHolder` owns
    // that, and a caller needs both.
    bool describes(const Shape* receiverShape) const noexcept {
        return isRealShape() && cached_shape == receiverShape &&
               (realDepth() == 0 || cached_epoch == protoMutationEpoch());
    }

    // The same question for a WRITE, which additionally requires depth 0: the
    // slot is written on the RECEIVER, so an entry naming an ancestor's slot
    // would put the value in an unrelated own property. Set sites and get
    // sites never share a table entry, so no entry a write path sees has ever
    // been filled at depth > 0 — this asks anyway, because that is a fact
    // about the compiler's site numbering and this is the runtime.
    bool describesOwn(const Shape* receiverShape) const noexcept {
        return isRealShape() && !isAccessor() && cached_depth == 0 && cached_shape == receiverShape;
    }

    void fill(Shape* receiverShape, uint32_t slot, uint32_t depth) noexcept {
        cached_shape = receiverShape;
        cached_slot = slot;
        cached_depth = depth;
        cached_epoch = protoMutationEpoch();
    }

    void fillAccessor(Shape* receiverShape, uint32_t slot, uint32_t depth) noexcept {
        cached_shape = receiverShape;
        cached_slot = slot;
        cached_depth = depth | BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG;
        cached_epoch = protoMutationEpoch();
    }

    void fillArrayMethod(uint32_t methodId) noexcept {
        cached_shape = reinterpret_cast<Shape*>(BRONZE_ABI_IC_SHAPE_ARRAY_METHOD);
        cached_slot = methodId;
        cached_depth = 0;
        cached_epoch = 0;
    }
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

    // `shape` is required: it decides the object's prototype, and minting a
    // fresh root shape per object would give every `{}` literal an unrelated
    // hidden class.
    static ObjectHeader* create(Heap& heap, NonMovingArena& arena, Shape* shape);

    // The same object with INTERNAL SLOTS (ECMA-262 6.1.7.2): `count` Values
    // the object carries that are not properties and cannot be reached as any.
    //
    // They live immediately after the inline property slots, where the property
    // path cannot address them: a slot index below `kInlineSlots` is an inline
    // property and every index at or above it is resolved against `overflow`,
    // so nothing a shape or a dictionary can name lands here. That is the whole
    // point. An internal slot is invisible to `Object.keys`, to `for-in`, to
    // `getOwnPropertyNames` AND to `getOwnPropertySymbols`; spelling one as a
    // property under a reserved name buys the first two and nothing else, which
    // is all the retired `@@` convention could ever do.
    //
    // The collector needs nothing added for them: it scans an object's payload
    // as an array of Values sized by `header.size`, so a wider block is traced
    // by the same walk that already traces the inline slots.
    static ObjectHeader* createWithInternalSlots(Heap& heap, NonMovingArena& arena, Shape* shape,
                                                 uint32_t count);

    // How many this object was created with — 0 for every ordinary object.
    // Part of the BRAND a runtime iterator checks its receiver with: an object
    // forged with `Object.create(<that prototype>)` was not allocated with
    // these, so it answers 0 and is refused rather than read past its end.
    uint32_t internalSlotCount() const noexcept;
    Value internalSlot(uint32_t index) const;
    void setInternalSlot(uint32_t index, Value val);

    // The object `cached_depth` prototype links up from this one, or null
    // if the chain is shorter than that. No allocation, so the raw pointer
    // is safe to the next allocation.
    ObjectHeader* protoAncestor(uint32_t depth) noexcept;

    // The same walk, made safe for an INLINE CACHE to read a slot off the
    // end of. An entry is `(shape, slot, depth)`, and the receiver's shape is
    // all it checks — so nothing an entry holds notices a change to an object
    // between the receiver and the holder. Two such changes exist: a delete
    // (which renumbers slots) and a prototype swap (which replaces the holder
    // entirely), and BOTH put the object they touch into dictionary mode. So
    // this refuses the walk if any link it crosses — the holder included — is
    // a dictionary, which turns both into a cache miss and a correct slow
    // lookup.
    //
    // `crossedDictionary` separates the two ways to answer null: a chain that
    // is genuinely shorter than `depth` is impossible (the prototype lives on
    // the shape, so it cannot change without the shape changing) and stays a
    // tripwire; a dictionary on the path is ordinary and expected.
    //
    // The third change — an ADD to an intermediate, which takes a shape
    // transition and leaves no dictionary behind — is NOT visible here and
    // cannot be: this walk sees the chain as it is now, not as it was when the
    // entry was filled. `cached_epoch` is what covers it, and the two
    // mechanisms are kept separate because they answer different questions —
    // "is this walk safe to take" and "is this entry still about the same
    // chain".
    ObjectHeader* cachedProtoHolder(uint32_t depth, bool& crossedDictionary) noexcept;

    // `Object.setPrototypeOf`. The prototype lives on the shape's ROOT, which
    // every object sharing that root shares — so a swap cannot write through it
    // and must give this object a shape of its own. Dictionary mode is that
    // shape, and it is not merely convenient: a dictionary is what
    // `cachedProtoHolder` above refuses, so an entry that walks THROUGH this
    // object stops hitting the moment the swap happens.
    static void setPrototype(NonMovingArena& arena, Rooted<Value>& self, class Shape* newRoot);

    // May allocate and may run USER CODE: a property whose own-or-inherited
    // definition is an accessor calls its getter here. So `this` must be
    // reachable from a root at the call and must not be reused afterwards — the
    // same contract setProp has always had.
    //
    // `receiver` is what `this` is bound to if the property turns out to be an
    // accessor: the object itself for every ordinary read, which is why it
    // defaults to null. A function's STATIC members live in a side object, so
    // that one caller passes the constructor down and a static getter sees the
    // class rather than the box its properties are kept in.
    Value getProp(Heap& heap, Rooted<Value>& key, InlineCache* ic = nullptr,
                  const Value* receiver = nullptr);
    // May allocate (overflow growth), which can move this object; use the
    // returned pointer afterwards, not `this`. May also run user code, for
    // the same reason getProp can: an inherited setter.
    //
    // `enumerable` decides the ATTRIBUTE a newly created property gets, and is
    // therefore part of the shape transition it takes. It is false for exactly
    // one caller — a class method definition — and an ordinary assignment never
    // reaches it, because assignment always creates an enumerable property.
    //
    // `defineOwn` switches from Set (ECMA-262 10.1.9) to DefineOwnProperty
    // (10.1.6): a definition never runs an inherited setter. A class method
    // and an object spread are definitions; `o.k = v` is not, and the
    // difference is only observable now that a prototype can carry one.
    //
    // `refused` reports the three ways ECMA-262 10.1.9.2 can answer FALSE. The
    // answer is discarded by a sloppy assignment and turned into a TypeError by
    // a strict one (13.15.2 PutValue step 6.d), so the reason has to survive
    // the call — and it is an enum rather than a bool because the three want
    // three different messages, and "the write did not happen" with no reason
    // named is the diagnostic that sends a reader nowhere.
    ObjectHeader* setProp(Heap& heap, NonMovingArena& arena, Rooted<Value>& key,
                          Rooted<Value>& val, InlineCache* ic = nullptr,
                          bool enumerable = true, bool defineOwn = false,
                          const Value* receiver = nullptr, SetRefusal* refused = nullptr,
                          bool writable = true, bool configurable = true);

    // `delete o.k`, which removes an OWN property and answers true — also when
    // the property was never there, and when only a prototype has it (ECMA-262
    // 13.5.1 / 10.5.6). Removing from the middle of a transition chain is
    // impossible, so the first successful delete moves the object to dictionary
    // mode. Allocates nothing on the heap; the dictionary and its shape live in
    // the arena.
    bool deleteProperty(NonMovingArena& arena, PropertyKey name);

    // `get k() {}` / `set k(v) {}`. Defines ONE property with two halves: a
    // second call for the other half of the same name updates the pair
    // rather than creating a second property. Allocates (a shape transition
    // may grow the overflow block), so it takes the object through a root
    // and the caller must re-derive any raw pointer afterwards.
    // `configurable` defaults true because the language forms that reach here
    // — literal accessors, class accessors, __defineGetter__ — all produce
    // configurable properties; Object.defineProperty passes what its
    // descriptor says (absent means FALSE there, 6.2.6.5).
    static void defineAccessor(Heap& heap, NonMovingArena& arena, Rooted<Value>& self,
                               Rooted<Value>& key, Rooted<Value>& getter, Rooted<Value>& setter,
                               bool enumerable, bool configurable = true);

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
                                    PropertyKey name, bool enumerable, bool accessor,
                                    uint32_t& out_slot);

    // Grow the out-of-line block so that `needed` overflow slots — or, for
    // ensureSlots, slot indices [0, count) — are addressable. Allocates, so
    // the object is reached through the root and `this` must not be reused.
    static ObjectHeader* ensureOverflow(Heap& heap, Rooted<Value>& self, uint32_t needed);
    static ObjectHeader* ensureSlots(Heap& heap, Rooted<Value>& self, uint32_t count);

    inline Value getSlot(uint32_t index) const {
        if (index < kInlineSlots) {
            return slotsData()[index];
        }
        uint32_t oi = index - kInlineSlots;
        if (oi >= overflowCapacity()) {
            fatal("object slot index beyond overflow capacity (corrupt shape?)");
        }
        return overflow.asObject<HeapObjectHeader>()->payload<Value>()[oi];
    }

    inline void setSlot(uint32_t index, Value val) {
        if (index < kInlineSlots) {
            slotsData()[index] = val;
            return;
        }
        uint32_t oi = index - kInlineSlots;
        if (oi >= overflowCapacity()) {
            fatal("object slot index beyond overflow capacity (corrupt shape?)");
        }
        overflow.asObject<HeapObjectHeader>()->payload<Value>()[oi] = val;
    }

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

// These two layouts are part of the generated-code ABI: compiled code loads the
// shape word and the cache entry itself rather than calling a helper to do it.
// The constants live in bronze_abi.h, which is pure C and cannot see a C++
// class, so this is where the two sides are tied together — deliberately in the
// HEADER, so every translation unit that can see the structs also checks them.
// Adding or reordering a field breaks the build here instead of miscompiling
// every property read.
static_assert(sizeof(InlineCache) == BRONZE_ABI_IC_ENTRY_SIZE);
static_assert(alignof(InlineCache) <= 8);
static_assert(offsetof(InlineCache, cached_shape) == BRONZE_ABI_IC_SHAPE_OFFSET);
static_assert(offsetof(InlineCache, cached_slot) == BRONZE_ABI_IC_SLOT_OFFSET);
static_assert(offsetof(InlineCache, cached_depth) == BRONZE_ABI_IC_DEPTH_OFFSET);
static_assert(offsetof(InlineCache, cached_epoch) == BRONZE_ABI_IC_EPOCH_OFFSET);
static_assert(sizeof(InlineCache::cached_slot) == 4 && sizeof(InlineCache::cached_depth) == 4,
              "the fast path reads slot and depth as one u64; both halves must be 32 bits");

static_assert(offsetof(HeapObjectHeader, flags) == BRONZE_ABI_OBJ_FLAGS_OFFSET);
static_assert(offsetof(HeapObjectHeader, size) == BRONZE_ABI_HDR_SIZE_OFFSET);
// Generated code inlines the plain-object check (llvm_prop.cpp), so this one
// kind's number is part of the ABI and the registry in heap.h must agree with
// it. The other kinds are runtime-internal and free to move.
static_assert(HeapKind::Plain == BRONZE_ABI_OBJ_FLAGS_PLAIN);
static_assert(HeapKind::Array == BRONZE_ABI_OBJ_FLAGS_ARRAY);
static_assert(HeapKind::TypedArray == BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY);
static_assert(offsetof(ObjectHeader, shape) == BRONZE_ABI_OBJ_SHAPE_OFFSET);
static_assert(offsetof(ObjectHeader, overflow) == BRONZE_ABI_OBJ_OVERFLOW_OFFSET);
static_assert(sizeof(ObjectHeader) == BRONZE_ABI_OBJ_SLOTS_OFFSET,
              "inline slots start immediately after ObjectHeader");
static_assert(ObjectHeader::kInlineSlots == BRONZE_ABI_OBJ_INLINE_SLOTS);

}  // namespace bronze
