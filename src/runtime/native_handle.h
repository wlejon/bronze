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
// A handle is a plain object with FIVE internal slots, invisible to every
// property walk: the data pointer, the destructor (duplicated into the
// finalizer registry so the sweep never reads a payload), the brand word
// that makes rtHandleData refuse every other internal-slot object, the
// CLASS TAG — an opaque pointer a native class's registration owns, or a
// host class's brand (rtMakeBrandedHandle), null for a handle made by
// makeHandle — and the realm it was made in. A native method compares its
// receiver's tag against its class's tag: one load and one compare, and a
// TypeError naming the class when they differ.

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
//
// A handle made in one realm unwraps only in that realm (or anywhere, for one
// made before any realm existed): in another this answers nullptr, exactly as
// for a value of the wrong kind, so a host's natives refuse what another app
// made. rtHandleDataAnyRealm is the host's own unchecked read (teardown,
// registries keyed by payload).
void* rtHandleData(Value handle);
void* rtHandleDataAnyRealm(Value handle);
// The realm slot the handle was made in (0 for none, or a non-handle), and
// whether that is another realm than the current one.
uint32_t rtHandleRealmSlot(Value handle);
bool rtHandleForeign(Value handle);
bool rtIsHandle(Value handle);
// The class tag the cell was made with, or nullptr for a non-handle, an
// untagged handle, or a handle carrying a host brand instead.
const void* rtHandleClassTag(Value handle);

// A handle whose class-tag slot carries a HOST BRAND rather than a native
// class tag: an embed::HostClass names itself there (embed_host_class.cpp),
// so unwrap reads its brand out of the cell, with no table and no lock, and
// the brand lives exactly as long as the handle. The slot holds the brand with
// its low bit set (a HostClass is at least 2-byte aligned), which no native
// class tag has, so rtHandleClassTag never answers a brand and
// bronze_native_handle_data never takes one for a NativeClassInfo.
Value rtMakeBrandedHandle(void* data, HandleDestructor dtor, Finalize when, Value prototype,
                          const void* brand);
// The host brand the cell was made with, or nullptr for a non-handle or a
// handle without one.
const void* rtHandleBrand(Value handle);
// rtHandleData and rtHandleBrand in one look at the cell (a host class's
// unwrap): the data pointer exactly as rtHandleData answers it, and the
// brand through `brand` (nullptr whenever the data is).
void* rtHandleDataAndBrand(Value handle, const void** brand);

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
