// The two ways a host keeps state alive across the moving collector:
// Persistent (a rooted slot the collector updates in place) and the opaque
// native handle (a heap cell owning a host pointer, destroyed when the cell
// dies). Both hang off registries in this file, and the finalizer sweep those
// handles need is the one consumer of the Heap's post-collection hook.

#include <cstdint>
#include <string>
#include <vector>

#include "embed/embed.h"
#include "embed/embed_internal.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/value.h"

namespace bronze::embed {

namespace {

// ---- registries ------------------------------------------------------------

// Persistent slots. Free slots hold undefined and are recycled through the
// free list; the root source visits every slot, which costs one no-op visit
// per free slot and keeps the source a plain loop.
thread_local std::vector<Value> g_persistentSlots;
thread_local std::vector<uint32_t> g_persistentFreeSlots;

// The native-handle finalizer registry: one entry per live handle cell. The
// collector never visits a dead object — it copies the live ones and abandons
// the rest — so "run the destructor when the cell dies" can only be answered
// from outside, by sweeping this table when a collection has just decided
// liveness for everything (heap.h, set_post_collection_hook).
struct FinalizerEntry {
    HeapObjectHeader* cell;  // the handle cell's CURRENT header address
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

// The handle cells' own root shape. Null prototype on purpose, like the
// namespace objects' root shapes: a handle shares no transition tree with
// `{}` literals, and a chain walk over one ends immediately — until a host
// gives it a prototype, which is in-contract (see handleData). Creation-time
// only: the brand does NOT live here, precisely because a prototype swap
// re-roots the shape.
thread_local Shape* g_handleShape = nullptr;

// The brand handleData checks, stored in the cell itself (internal slot 2) so
// no shape operation can detach it: `Object.setPrototypeOf` converts the cell
// to dictionary mode and repoints its shape root, which is exactly what a
// shape-root brand would not survive. Top 16 bits zero, so the collector's
// payload scan reads it as a non-pointer, like the two pointers beside it.
// Unforgeable in practice: internal slots are unreachable from JS, and no
// other internal-slot creator in the runtime writes this word.
constexpr uint64_t kHandleBrandBits = 0x0000'B805'EAD1'C377ULL;

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
            // Dead, Deferred: queue for drainFinalizers. Only the host pair
            // survives — the cell is from-space garbage the moment this sweep
            // returns, which is precisely what licenses the destructor to do
            // anything it likes later: there is no heap state left to
            // resurrect or corrupt.
            g_pendingFinalizers.push_back({entry.data, entry.dtor});
        }
    }
    g_finalizers.resize(keep);
}

// Registration with the collector, on FIRST USE rather than at static
// initialization: the heap and its root-source table are statics of another
// translation unit (rt_state.cpp), and registering from this TU's static
// initializers would race them — the exact cross-TU-order trap rt_state.cpp
// exists to close. By the time a host calls anything here, main() has begun
// and the runtime's statics are long constructed.
void ensureRegistries() {
    static thread_local const bool registered = [] {
        runtime::rtHeap().add_root_source([](const Heap::RootVisitor& visit) {
            for (Value& slot : g_persistentSlots) visit(slot);
        });
        // One of the hook LIST's entries now (heap.h says why it stopped being
        // a slot): the weak-reference sweep registers its own, and the two are
        // independent — this one only ever reads handle cells.
        runtime::rtHeap().add_post_collection_hook(sweepFinalizers);
        return true;
    }();
    (void)registered;
}

uint32_t acquireSlot(Value v) {
    ensureRegistries();
    if (!g_persistentFreeSlots.empty()) {
        uint32_t slot = g_persistentFreeSlots.back();
        g_persistentFreeSlots.pop_back();
        g_persistentSlots[slot] = v;
        return slot;
    }
    g_persistentSlots.push_back(v);
    return static_cast<uint32_t>(g_persistentSlots.size() - 1);
}

void releaseSlot(uint32_t slot) {
    g_persistentSlots[slot] = Value::fromUndefined();
    g_persistentFreeSlots.push_back(slot);
}

// A host pointer stored raw in an internal slot must never look like a heap
// reference to the collector's payload scan. It cannot: forwarding only
// touches Values whose TAG is Object/String/Symbol (top 16 bits), and a
// user-space pointer's top 16 bits are zero, which reads as a small double.
// The check makes the assumption loud instead of latent.
uint64_t pointerBits(const void* p, const char* what) {
    auto bits = reinterpret_cast<uint64_t>(p);
    if (bits > kPayloadMask) {
        fatal((std::string("embed: a ") + what +
               " pointer above 2^48, which the value model cannot carry raw")
                  .c_str());
    }
    return bits;
}

}  // namespace

// ---- Persistent ------------------------------------------------------------

Persistent::Persistent() : slot_(acquireSlot(Value::fromUndefined())) {}

Persistent::Persistent(Value v) : slot_(acquireSlot(v)) {}

Persistent::~Persistent() {
    if (slot_ != kNoSlot) releaseSlot(slot_);
}

Persistent::Persistent(const Persistent& other)
    : slot_(acquireSlot(other.slot_ != kNoSlot ? g_persistentSlots[other.slot_]
                                               : Value::fromUndefined())) {}

Persistent& Persistent::operator=(const Persistent& other) {
    if (this != &other) {
        set(other.get());
    }
    return *this;
}

Persistent::Persistent(Persistent&& other) noexcept : slot_(other.slot_) {
    other.slot_ = kNoSlot;
}

Persistent& Persistent::operator=(Persistent&& other) noexcept {
    if (this != &other) {
        if (slot_ != kNoSlot) releaseSlot(slot_);
        slot_ = other.slot_;
        other.slot_ = kNoSlot;
    }
    return *this;
}

Value Persistent::get() const {
    // Moved-from answers undefined rather than tripping: reading a moved-from
    // handle is host code that compiles either way, and undefined is the
    // answer that fails soft in JS terms.
    return slot_ != kNoSlot ? g_persistentSlots[slot_] : Value::fromUndefined();
}

void Persistent::set(Value v) {
    if (slot_ == kNoSlot) {
        slot_ = acquireSlot(v);
    } else {
        g_persistentSlots[slot_] = v;
    }
}

// ---- opaque native handles -------------------------------------------------

namespace {

// The shared tail of both makeHandle forms, from the shape on: the caller has
// already opened a frame and picked the root shape, and NOTHING between the
// shape choice and here allocates — the shape's prototype slot is forwarded by
// the root-shape registry, so it alone survives whatever the create below does.
Value makeHandleOnShape(void* data, HandleDestructor dtor, Finalize when, Shape* shape) {
    // Internal slots, not properties: invisible to Object.keys, for-in and the
    // symbol walk, which is the whole meaning of "opaque". Slot 0 is the data
    // pointer, slot 1 the destructor — duplicated into the registry entry so
    // the sweep never reads a payload (see sweepFinalizers) — and slot 2 the
    // brand, in the cell rather than on the shape so it survives everything a
    // shape does not.
    ObjectHeader* cell = ObjectHeader::createWithInternalSlots(runtime::rtHeap(),
                                                              runtime::rtArena(),
                                                              shape, 3);
    cell->setInternalSlot(0, Value::fromRawBits(pointerBits(data, "handle data")));
    cell->setInternalSlot(1,
                          Value::fromRawBits(pointerBits(reinterpret_cast<void*>(dtor),
                                                         "handle destructor")));
    cell->setInternalSlot(2, Value::fromRawBits(kHandleBrandBits));
    // No allocation between the create above and this push, so `cell` cannot
    // have moved: the entry records the address the collector will next see.
    g_finalizers.push_back({&cell->header, data, dtor, when});
    return Value::fromObject(cell);
}

}  // namespace

Value makeHandle(void* data, HandleDestructor dtor, Finalize when) {
    if (!dtor) fatal("embed: a native handle needs a destructor (pass a no-op explicitly)");
    ShadowStackFrame frame;
    ensureRegistries();
    if (!g_handleShape) {
        g_handleShape = runtime::rtNewRootShape(Value::fromNull());
    }
    return makeHandleOnShape(data, dtor, when, g_handleShape);
}

Value makeHandle(void* data, HandleDestructor dtor, Finalize when, Value prototype) {
    if (!dtor) fatal("embed: a native handle needs a destructor (pass a no-op explicitly)");
    // The prototype must be a plain object, for Object.create's reason: a walk
    // that misses on the instance steps INTO this value expecting a shape to
    // look through, and only a plain object carries one. Not silently the
    // 3-argument form — a host that names a prototype means it.
    if (!prototype.isObject() ||
        prototype.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) {
        fatal("embed: a handle's prototype must be a plain object "
              "(use the 3-argument makeHandle for a bare cell)");
    }
    ShadowStackFrame frame;
    ensureRegistries();
    // The memoized per-prototype root shape Object.create hands out — so every
    // handle of one class shares a transition tree and the inline caches that
    // come with it, which is the point of being born on the prototype instead
    // of setPrototypeOf'd onto it (that route is dictionary mode forever).
    return makeHandleOnShape(data, dtor, when,
                             runtime::rtRootShapeForPrototype(prototype));
}

// embed_internal.h — the registry, opened to the module's other translation
// units: an external buffer's release rides the same sweep a handle's
// destructor does, and a second registry would be the drift heap.h's hook
// LIST exists to prevent.
void registerHeapFinalizer(HeapObjectHeader* cell, void* data, HandleDestructor dtor,
                           Finalize when) {
    ensureRegistries();
    g_finalizers.push_back({cell, data, dtor, when});
}

void drainFinalizers() {
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

bool finalizersPending() { return !g_pendingFinalizers.empty(); }

void* handleData(Value handle) {
    if (!handle.isObject()) return nullptr;
    auto* hdr = handle.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Plain) return nullptr;
    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    // The brand is the slot count plus the brand word — deliberately NOT the
    // shape root (the iterator objects' pattern), because a handle's shape is
    // mutable in-contract: `Object.setPrototypeOf(handle, proto)` is how a
    // wrapper layer gives every instance a shared method prototype instead of
    // per-instance closures, and that swap re-roots the shape. The internal
    // slots it cannot touch: they live past the inline property slots, and
    // property storage grows into the overflow block, never over them.
    if (obj->internalSlotCount() != 3 ||
        obj->internalSlot(2).rawBits() != kHandleBrandBits) {
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(obj->internalSlot(0).rawBits()));
}

}  // namespace bronze::embed
