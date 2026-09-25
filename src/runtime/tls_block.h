#pragma once

#include "abi/bronze_abi.h"
#include <cstddef>

// The per-thread ABI block, reachable WITHOUT a call.
//
// `bronze_tls_block_addr` is the ABI: generated code calls it once per
// function prologue and LLVM CSEs the result, so from that side the call is
// paid once and hoisted out of every loop. The runtime's own side is the
// opposite shape — a dozen one-line predicates (`elemCacheEnabled`,
// `rtIcWayLimit`, `directCalloutEnabled`, the root-block seam) each fetch the
// block, read one word and return, and every one of those fetches was a call
// through the DLL's own export thunk. The chunk-4 sampler measured
// `bronze_tls_block_addr` at 0.94 % of the `many_meshes` frame, which is what
// a call that should have been three instructions costs at that rate.
//
// So the block is a namespace-scope `thread_local` and this is the inline way
// in. `bronze_tls_block_addr` still exists, still returns this address, and is
// still the only thing generated code and hosts may use: what changed is that
// code compiled INTO the runtime image no longer goes through it.
//
// The initializer is a constant aggregate, so the variable is
// constant-initialized and neither route pays an init guard.

namespace bronze::runtime {

extern thread_local bronze_tls_block g_tls_block;

inline bronze_tls_block* rtTls() noexcept {
    return &g_tls_block;
}

// Reads every BRONZE_NO_* seam into the calling thread's block
// (thread_seams.cpp). Heap's constructor calls it, so it runs at a thread's
// first touch of the runtime.
void rtReadThreadSeams();

void setShadowStackCapacityForTesting(size_t words);
void resetShadowStackCapacityForTesting();
bool isShadowStackFrame(const bronze_gc_frame* frame);
size_t shadowStackCapacityWords();

}  // namespace bronze::runtime
