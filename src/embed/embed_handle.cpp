// The two ways a host keeps state alive across the moving collector:
// Persistent (a rooted slot the collector updates in place) and the opaque
// native handle (a heap cell owning a host pointer, destroyed when the cell
// dies). The Persistent and HandleScope registries live here; the handle
// cells and their finalizer sweep live in the runtime (native_handle.cpp),
// because generated code makes and unwraps handles too (a native
// constructor's result, a native method's receiver), and this file is the
// host-facing spelling of the same calls.

#include <cstdint>
#include <string>
#include <vector>

#include "embed/embed.h"
#include "embed/embed_internal.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/native_handle.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

namespace bronze::embed {

namespace {

// ---- registries ------------------------------------------------------------

// Persistent slots. Free slots hold undefined and are recycled through the
// free list; the root source visits every slot, which costs one no-op visit
// per free slot and keeps the source a plain loop.
//
// The registries are thread_locals, and a thread's thread_locals are destroyed
// before the process's statics (exit() runs the TLS destructors first). A host
// that keeps a Persistent in a static (a module-level callback holder) has
// its destructor run after the slot vectors are gone, so releasing into them
// would write freed memory. `g_registriesGone` is trivially destructible, so
// it stays readable for the rest of the thread's life; once it is set a
// Persistent's release is a no-op (the slots it would free no longer exist)
// and a read answers undefined.
thread_local bool g_registriesGone = false;

struct Registries {
    std::vector<Value> persistent;
    std::vector<uint32_t> persistentFree;
    std::vector<Value> local;
    ~Registries() { g_registriesGone = true; }
};
thread_local Registries g_registries;
// Persistent::kNoSlot, the moved-from marker.
constexpr uint32_t kNoPersistentSlot = UINT32_MAX;
thread_local HandleScope* g_currentHandleScope = nullptr;

// Registration with the collector, on FIRST USE rather than at static
// initialization: the heap and its root-source table are statics of another
// translation unit (rt_state.cpp), and registering from this TU's static
// initializers would race them — the exact cross-TU-order trap rt_state.cpp
// exists to close. By the time a host calls anything here, main() has begun
// and the runtime's statics are long constructed.
void ensureRegistries() {
    static thread_local const bool registered = [] {
        runtime::rtHeap().add_root_source([](const Heap::RootVisitor& visit) {
            if (g_registriesGone) return;
            for (Value& slot : g_registries.persistent) visit(slot);
            for (Value& slot : g_registries.local) visit(slot);
        });
        return true;
    }();
    (void)registered;
}

uint32_t acquireSlot(Value v) {
    // During the thread's teardown: the handle behaves as moved-from.
    if (g_registriesGone) return kNoPersistentSlot;
    ensureRegistries();
    if (!g_registries.persistentFree.empty()) {
        uint32_t slot = g_registries.persistentFree.back();
        g_registries.persistentFree.pop_back();
        g_registries.persistent[slot] = v;
        return slot;
    }
    g_registries.persistent.push_back(v);
    return static_cast<uint32_t>(g_registries.persistent.size() - 1);
}

void releaseSlot(uint32_t slot) {
    if (g_registriesGone) return;
    g_registries.persistent[slot] = Value::fromUndefined();
    g_registries.persistentFree.push_back(slot);
}

}  // namespace

// ---- Persistent ------------------------------------------------------------

Persistent::Persistent() : slot_(acquireSlot(Value::fromUndefined())) {}

Persistent::Persistent(Value v) : slot_(acquireSlot(v)) {}

Persistent::~Persistent() {
    if (slot_ != kNoSlot) releaseSlot(slot_);
}

Persistent::Persistent(const Persistent& other)
    : slot_(acquireSlot(other.get())) {}

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
    return slot_ != kNoSlot && !g_registriesGone ? g_registries.persistent[slot_]
                                                 : Value::fromUndefined();
}

void Persistent::set(Value v) {
    if (g_registriesGone) return;
    if (slot_ == kNoSlot) {
        slot_ = acquireSlot(v);
    } else {
        g_registries.persistent[slot_] = v;
    }
}

// ---- local handle scopes ---------------------------------------------------

uint32_t createLocalSlot(Value v) {
    ensureRegistries();
    if (!g_currentHandleScope) {
        fatal("embed: Local handle created without an active HandleScope");
    }
    g_registries.local.push_back(v);
    return static_cast<uint32_t>(g_registries.local.size() - 1);
}

Value getLocalValue(uint32_t index) {
    if (index >= g_registries.local.size()) return Value::fromUndefined();
    return g_registries.local[index];
}

void setLocalValue(uint32_t index, Value v) {
    if (index < g_registries.local.size()) {
        g_registries.local[index] = v;
    }
}

HandleScope::HandleScope() {
    ensureRegistries();
    prev_top_ = static_cast<uint32_t>(g_registries.local.size());
    prev_scope_ = g_currentHandleScope;
    g_currentHandleScope = this;
}

HandleScope::~HandleScope() {
    g_registries.local.resize(prev_top_);
    g_currentHandleScope = prev_scope_;
}

size_t HandleScope::numberOfHandles() const noexcept {
    return g_registries.local.size() >= prev_top_ ? (g_registries.local.size() - prev_top_) : 0;
}

EscapableHandleScope::EscapableHandleScope() : HandleScope() {}

EscapableHandleScope::~EscapableHandleScope() = default;

uint32_t EscapableHandleScope::escapeSlot(uint32_t index) {
    if (escaped_) {
        fatal("embed: EscapableHandleScope::escape called more than once on the same scope");
    }
    if (index >= g_registries.local.size()) {
        fatal("embed: EscapableHandleScope::escape passed invalid handle index");
    }
    escaped_ = true;
    Value v = g_registries.local[index];
    if (prev_top_ < g_registries.local.size()) {
        g_registries.local[prev_top_] = v;
    } else {
        g_registries.local.push_back(v);
    }
    uint32_t ret_index = prev_top_;
    prev_top_++;
    return ret_index;
}

// ---- opaque native handles -------------------------------------------------

Value makeHandle(void* data, HandleDestructor dtor, Finalize when) {
    if (!dtor) fatal("embed: a native handle needs a destructor (pass a no-op explicitly)");
    return runtime::rtMakeHandle(data, dtor, when, Value::fromNull(), nullptr);
}

Value makeHandle(void* data, HandleDestructor dtor, Finalize when, Value prototype) {
    if (!dtor) fatal("embed: a native handle needs a destructor (pass a no-op explicitly)");
    // Not silently the 3-argument form — a host that names a prototype means
    // it, so anything but a plain object is a named fatal (the runtime's own
    // check, reached through rtMakeHandle, says the same for a non-plain
    // object; this one catches the non-object).
    if (!prototype.isObject()) {
        fatal("embed: a handle's prototype must be a plain object "
              "(use the 3-argument makeHandle for a bare cell)");
    }
    return runtime::rtMakeHandle(data, dtor, when, prototype, nullptr);
}

// embed_internal.h — the registry, opened to the module's other translation
// units: an external buffer's release rides the same sweep a handle's
// destructor does, and a second registry would be the drift heap.h's hook
// LIST exists to prevent.
void registerHeapFinalizer(HeapObjectHeader* cell, void* data, HandleDestructor dtor,
                           Finalize when) {
    runtime::rtRegisterHeapFinalizer(cell, data, dtor, when);
}

void drainFinalizers() { runtime::rtDrainFinalizers(); }

bool finalizersPending() { return runtime::rtFinalizersPending(); }

void* handleData(Value handle) { return runtime::rtHandleData(handle); }

}  // namespace bronze::embed
