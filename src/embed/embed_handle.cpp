// The two ways a host keeps state alive across the moving collector:
// Persistent (a rooted slot the collector updates in place) and the opaque
// native handle (a heap cell owning a host pointer, destroyed when the cell
// dies). Both hang off registries in this file, and the finalizer sweep those
// handles need is the one consumer of the Heap's post-collection hook.

#include <cstdint>
#include <string>
#include <vector>

#include "embed/embed.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/value.h"

namespace bronze::embed {

namespace {

// ---- registries ------------------------------------------------------------

// Persistent slots. Free slots hold undefined and are recycled through the
// free list; the root source visits every slot, which costs one no-op visit
// per free slot and keeps the source a plain loop.
std::vector<Value> g_persistentSlots;
std::vector<uint32_t> g_persistentFreeSlots;

// The native-handle finalizer registry: one entry per live handle cell. The
// collector never visits a dead object — it copies the live ones and abandons
// the rest — so "run the destructor when the cell dies" can only be answered
// from outside, by sweeping this table when a collection has just decided
// liveness for everything (heap.h, set_post_collection_hook).
struct FinalizerEntry {
    HeapObjectHeader* cell;  // the handle cell's CURRENT header address
    void* data;
    HandleDestructor dtor;
};
std::vector<FinalizerEntry> g_finalizers;

// The handle cells' own root shape, and the brand handleData checks. Null
// prototype on purpose, like the namespace objects' root shapes: a handle
// shares no transition tree with `{}` literals, and a chain walk over one
// ends immediately.
Shape* g_handleShape = nullptr;

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
        } else {
            // Dead. The destructor gets the pointer the registry duplicated at
            // creation rather than one read out of the dead payload — the
            // forwarding protocol clobbers a payload's first word for LIVE
            // objects, and a sweep that read payloads would have to know it is
            // on the safe side of that. It must not touch the bronze heap: the
            // collection is mid-flight (heap.h).
            entry.dtor(entry.data);
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
    static const bool registered = [] {
        runtime::rtHeap().add_root_source([](const Heap::RootVisitor& visit) {
            for (Value& slot : g_persistentSlots) visit(slot);
        });
        // The hook is a single slot and this module is its one consumer; a
        // second consumer means widening the hook to a list, not chaining
        // around this one.
        runtime::rtHeap().set_post_collection_hook(sweepFinalizers);
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

Value makeHandle(void* data, HandleDestructor dtor) {
    if (!dtor) fatal("embed: a native handle needs a destructor (pass a no-op explicitly)");
    ShadowStackFrame frame;
    ensureRegistries();
    if (!g_handleShape) {
        g_handleShape = runtime::rtNewRootShape(Value::fromNull());
    }
    // Internal slots, not properties: invisible to Object.keys, for-in and the
    // symbol walk, which is the whole meaning of "opaque". Slot 0 is the data
    // pointer, slot 1 the destructor — duplicated into the registry entry so
    // the sweep never reads a payload (see sweepFinalizers).
    ObjectHeader* cell = ObjectHeader::createWithInternalSlots(runtime::rtHeap(),
                                                              runtime::rtArena(),
                                                              g_handleShape, 2);
    cell->setInternalSlot(0, Value::fromRawBits(pointerBits(data, "handle data")));
    cell->setInternalSlot(1,
                          Value::fromRawBits(pointerBits(reinterpret_cast<void*>(dtor),
                                                         "handle destructor")));
    // No allocation between the create above and this push, so `cell` cannot
    // have moved: the entry records the address the collector will next see.
    g_finalizers.push_back({&cell->header, data, dtor});
    return Value::fromObject(cell);
}

void* handleData(Value handle) {
    if (!handle.isObject()) return nullptr;
    auto* hdr = handle.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Plain) return nullptr;
    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    // The brand is the private root shape plus the slot count — the iterator
    // objects' pattern. A program that writes a property on a handle
    // transitions it off this shape and it stops answering here; the
    // finalizer still runs (the registry tracks the header, not the shape),
    // so opacity is a convention the host loses nothing by leaning on.
    if (obj->shape != g_handleShape || obj->internalSlotCount() != 2) return nullptr;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(obj->internalSlot(0).rawBits()));
}

}  // namespace bronze::embed
