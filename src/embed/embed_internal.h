#pragma once

// The embed module's own seams — declarations shared between its translation
// units and deliberately absent from embed.h, because no host has business
// calling them.

#include "embed/embed.h"
#include "runtime/heap.h"

namespace bronze::embed {

// Register `dtor(data)` to run when `cell` dies, through the same registry
// the opaque native handles use (embed_handle.cpp, sweepFinalizers). The cell
// may be ANY heap object — the external buffers register their
// ArrayBufferHeader here — and the entry tracks it across relocations exactly
// as a handle's is tracked. The caller must not allocate between obtaining
// `cell`'s address and this call, for the reason makeHandleOnShape documents:
// the entry records the address the collector will next see.
void registerHeapFinalizer(HeapObjectHeader* cell, void* data, HandleDestructor dtor,
                           Finalize when);

}  // namespace bronze::embed
