// The handle cells and the finalizer sweep they need — the one consumer of the
// Heap's post-collection hook list besides the weak-reference sweep. Moved
// here from embed_handle.cpp when generated code started making handles of
// its own (native_registry.cpp); the Persistent and HandleScope registries
// stayed there, because nothing below embed makes one.

#include "runtime/native_handle.h"

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"

namespace bronze::runtime {

namespace {

// The native-handle finalizer registry: one entry per live cell that owes a
// destructor. The collector never visits a dead object — it copies the live
// ones and abandons the rest — so "run the destructor when the cell dies" can
// only be answered from outside, by sweeping this table when a collection has
// just decided liveness for everything (heap.h, add_post_collection_hook).
struct FinalizerEntry {
    HeapObjectHeader* cell;  // the cell's CURRENT header address
    void* data;
    HandleDestructor dtor;
    Finalize when;
};
thread_local std::vector<FinalizerEntry> g_finalizers;

// Deferred destructors between the collection that queued them and the drain
// that runs them. Plain host pointers only — nothing here is a GC value, so
// the queue needs no rooting and survives any number of collections between
// checkpoints.
struct PendingFinalizer {
    void* data;
    HandleDestructor dtor;
};
thread_local std::vector<PendingFinalizer> g_pendingFinalizers;
thread_local bool g_drainingFinalizers = false;

// The bare handle cells' own root shape. Null prototype on purpose, like the
// namespace objects' root shapes: a bare handle shares no transition tree
// with `{}` literals, and a chain walk over one ends immediately — until a
// host gives it a prototype, which is in-contract (see rtHandleData).
// Creation-time only: the brand does NOT live here, precisely because a
// prototype swap re-roots the shape.
thread_local Shape* g_handleShape = nullptr;

// The brand rtHandleData checks, stored in the cell itself (internal slot 2)
// so no shape operation can detach it: `Object.setPrototypeOf` converts the
// cell to dictionary mode and repoints its shape root, which is exactly what
// a shape-root brand would not survive. Top 16 bits zero, so the collector's
// payload scan reads it as a non-pointer, like the pointers beside it.
// Unforgeable in practice: internal slots are unreachable from JS, and no
// other internal-slot creator in the runtime writes this word.
constexpr uint64_t kHandleBrandBits = 0x0000'B805'EAD1'C377ULL;
constexpr uint32_t kHandleSlotCount = 4;
constexpr uint32_t kSlotData = 0;
constexpr uint32_t kSlotDestructor = 1;
constexpr uint32_t kSlotBrand = 2;
constexpr uint32_t kSlotClassTag = 3;

void sweepFinalizers() {
    // Runs mid-collection: every from-space header is Tag::Forwarded (live,
    // new address in the payload) or untouched (dead). Stable compaction so
    // destructors run in registration order — not a promise the API makes,
    // but determinism costs one write index.
    size_t keep = 0;
    for (size_t i = 0; i < g_finalizers.size(); ++i) {
        FinalizerEntry& entry = g_finalizers[i];
        if (entry.cell->tag == static_cast<uint16_t>(Tag::Forwarded)) {
            entry.cell = *reinterpret_cast<HeapObjectHeader**>(entry.cell->payload());
            g_finalizers[keep++] = entry;
        } else if (entry.when == Finalize::InSweep) {
            // Dead. The destructor gets the pointer the registry duplicated at
            // creation rather than one read out of the dead payload — the
            // forwarding protocol clobbers a payload's first word for LIVE
            // objects, and a sweep that read payloads would have to know it is
            // on the safe side of that. It must not touch the bronze heap: the
            // collection is mid-flight (heap.h).
            entry.dtor(entry.data);
        } else {
            // Dead, Deferred: queue for the drain. Only the host pair
            // survives — the cell is from-space garbage the moment this sweep
            // returns, which is precisely what licenses the destructor to do
            // anything it likes later: there is no heap state left to
            // resurrect or corrupt.
            g_pendingFinalizers.push_back({entry.data, entry.dtor});
        }
    }
    g_finalizers.resize(keep);
}

// Registration with the collector on FIRST USE rather than at static
// initialization: the heap and its hook table are statics of another
// translation unit (rt_state.cpp), and registering from this TU's static
// initializers would race them — the exact cross-TU-order trap rt_state.cpp
// exists to close.
void ensureFinalizerHook() {
    static thread_local const bool registered = [] {
        // One of the hook LIST's entries (heap.h says why it stopped being a
        // slot): the weak-reference sweep registers its own, and the two are
        // independent — this one only ever reads handle cells.
        rtHeap().add_post_collection_hook(sweepFinalizers);
        return true;
    }();
    (void)registered;
}

// A host pointer stored raw in an internal slot must never look like a heap
// reference to the collector's payload scan. It cannot: forwarding only
// touches Values whose TAG is Object/String/Symbol (top 16 bits), and a
// user-space pointer's top 16 bits are zero, which reads as a small double.
// The check makes the assumption loud instead of latent.
uint64_t pointerBits(const void* p, const char* what) {
    auto bits = reinterpret_cast<uint64_t>(p);
    if (bits > kPayloadMask) {
        fatal((std::string("native handle: a ") + what +
               " pointer above 2^48, which the value model cannot carry raw")
                  .c_str());
    }
    return bits;
}

}  // namespace

Value rtMakeHandle(void* data, HandleDestructor dtor, Finalize when, Value prototype,
                   const void* classTag) {
    ShadowStackFrame frame;
    ensureFinalizerHook();
    Shape* shape = nullptr;
    if (prototype.isObject()) {
        // The prototype must be a plain object, for Object.create's reason: a
        // walk that misses on the instance steps INTO this value expecting a
        // shape to look through, and only a plain object carries one.
        if (prototype.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) {
            fatal("native handle: a handle's prototype must be a plain object");
        }
        // The memoized per-prototype root shape Object.create hands out — so
        // every handle of one class shares a transition tree and the inline
        // caches that come with it, which is the point of being born on the
        // prototype instead of setPrototypeOf'd onto it (that route is
        // dictionary mode forever).
        shape = rtRootShapeForPrototype(prototype);
    } else {
        if (!g_handleShape) g_handleShape = rtNewRootShape(Value::fromNull());
        shape = g_handleShape;
    }
    // NOTHING between the shape choice and the create below allocates: the
    // shape's prototype slot is forwarded by the root-shape registry, so it
    // alone survives whatever the create does.
    ObjectHeader* cell =
        ObjectHeader::createWithInternalSlots(rtHeap(), rtArena(), shape, kHandleSlotCount);
    cell->setInternalSlot(kSlotData, Value::fromRawBits(pointerBits(data, "handle data")));
    cell->setInternalSlot(kSlotDestructor,
                          Value::fromRawBits(pointerBits(reinterpret_cast<void*>(dtor),
                                                         "handle destructor")));
    cell->setInternalSlot(kSlotBrand, Value::fromRawBits(kHandleBrandBits));
    cell->setInternalSlot(kSlotClassTag, Value::fromRawBits(pointerBits(classTag, "class tag")));
    // No allocation between the create above and this push, so `cell` cannot
    // have moved: the entry records the address the collector will next see.
    if (dtor) g_finalizers.push_back({&cell->header, data, dtor, when});
    return Value::fromObject(cell);
}

namespace {

ObjectHeader* handleCell(Value handle) {
    if (!handle.isObject()) return nullptr;
    auto* hdr = handle.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Plain) return nullptr;
    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    // The brand is the slot count plus the brand word — deliberately NOT the
    // shape root (the iterator objects' pattern), because a handle's shape is
    // mutable in-contract. The internal slots it cannot touch: they live past
    // the inline property slots, and property storage grows into the overflow
    // block, never over them.
    if (obj->internalSlotCount() != kHandleSlotCount ||
        obj->internalSlot(kSlotBrand).rawBits() != kHandleBrandBits) {
        return nullptr;
    }
    return obj;
}

}  // namespace

void* rtHandleData(Value handle) {
    ObjectHeader* obj = handleCell(handle);
    if (!obj) return nullptr;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(obj->internalSlot(kSlotData).rawBits()));
}

bool rtIsHandle(Value handle) { return handleCell(handle) != nullptr; }

const void* rtHandleClassTag(Value handle) {
    ObjectHeader* obj = handleCell(handle);
    if (!obj) return nullptr;
    return reinterpret_cast<const void*>(
        static_cast<uintptr_t>(obj->internalSlot(kSlotClassTag).rawBits()));
}

void rtRegisterHeapFinalizer(HeapObjectHeader* cell, void* data, HandleDestructor dtor,
                             Finalize when) {
    ensureFinalizerHook();
    g_finalizers.push_back({cell, data, dtor, when});
}

void rtDrainFinalizers() {
    // Reentrancy is absorbed, not recursed: a Deferred destructor may
    // allocate, collect, and thereby queue more, and it may itself reach
    // drainMicrotasks — the inner drain returns and the loop below, indexing
    // rather than iterating, picks up whatever got appended.
    if (g_drainingFinalizers) return;
    g_drainingFinalizers = true;
    for (size_t i = 0; i < g_pendingFinalizers.size(); ++i) {
        // By copy: the vector may reallocate under a destructor's own
        // collections appending to it.
        PendingFinalizer pending = g_pendingFinalizers[i];
        pending.dtor(pending.data);
    }
    g_pendingFinalizers.clear();
    g_drainingFinalizers = false;
}

bool rtFinalizersPending() { return !g_pendingFinalizers.empty(); }

}  // namespace bronze::runtime
