#pragma once

// Opaque native handles: a heap cell owning a raw host pointer and a
// destructor, destroyed when the cell dies. The mechanism lives in the
// runtime rather than in embed because GENERATED CODE makes and unwraps
// these too — a native constructor's `void*` comes back through
// bronze_native_wrap, and a native method's receiver goes out through
// bronze_native_handle_data (native_registry.cpp) — and the runtime links
// nothing above itself. embed.h's makeHandle/handleData are the host-facing
// spelling of the same two calls and share every registry here.
//
// A handle is a plain object with FOUR internal slots, invisible to every
// property walk: the data pointer, the destructor (duplicated into the
// finalizer registry so the sweep never reads a payload), the brand word
// that makes rtHandleData refuse every other internal-slot object, and the
// CLASS TAG — an opaque pointer a native class's registration owns, null for
// a handle made by makeHandle. A native method compares its receiver's tag
// against its class's tag: one load and one compare, and a TypeError naming
// the class when they differ.

#include <cstdint>

#include "runtime/value.h"

namespace bronze {
struct HeapObjectHeader;
}

namespace bronze::runtime {

using HandleDestructor = void (*)(void* data);

// WHEN a destructor runs. embed.h carries the full contract for each.
enum class Finalize : uint8_t {
    InSweep,   // inside the collection that proved the cell dead
    Deferred,  // queued then, run at the next rtDrainFinalizers()
};

// `prototype` is a plain object the cell is born on, or null/undefined for a
// bare cell. `classTag` is stored verbatim; null means untagged. `dtor` may
// be null for a cell that owns nothing (a class registered without a
// destructor), in which case nothing is registered for it. Opens its own
// root frame; ALLOCATES.
Value rtMakeHandle(void* data, HandleDestructor dtor, Finalize when, Value prototype,
                   const void* classTag);

// The data pointer, or nullptr for a value that is not a handle. A brand
// check, not a shape check: `Object.setPrototypeOf(handle, p)` is in-contract.
void* rtHandleData(Value handle);
bool rtIsHandle(Value handle);
// The class tag the cell was made with, or nullptr for a non-handle or an
// untagged handle.
const void* rtHandleClassTag(Value handle);

// `dtor(data)` when `cell` dies, through the same registry the handles use.
// The cell may be ANY heap object (the external buffers register their
// ArrayBufferHeader). The caller must not allocate between obtaining `cell`
// and this call: the entry records the address the collector will next see.
void rtRegisterHeapFinalizer(HeapObjectHeader* cell, void* data, HandleDestructor dtor,
                             Finalize when);

// Run every Deferred destructor a collection has queued. Reentrancy-safe.
void rtDrainFinalizers();
bool rtFinalizersPending();

}  // namespace bronze::runtime
