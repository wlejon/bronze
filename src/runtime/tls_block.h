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

// ---- the stack limit and interrupts ------------------------------------------
//
// `stack_limit` is two things to generated code: the limit every prologue
// compares its stack pointer with, and the word every loop backedge compares
// it with too (brass's interrupt poll, Module::interrupt_poll). The runtime
// keeps the REAL limit of the running stack beside it, per stack
// (runtime/stack_locals.h: a compartment's fiber has its own), and arming an
// interrupt stores UINT64_MAX over the block's copy, which no stack pointer
// is above: the next prologue or backedge on that thread calls into the
// runtime (bronze_stack_overflow / bronze_interrupt_poll), which puts the
// real limit back and services the interrupt (rtServiceInterrupt).

inline constexpr uint64_t kInterruptArmed = UINT64_MAX;

// The running stack's real limit; 0 is none (no check).
uintptr_t rtStackLimit() noexcept;
// Sets the running stack's real limit (and the block's copy).
void rtSetStackLimit(uintptr_t limit) noexcept;
// The limit for a stack spanning [low, high): its low end plus the reserve
// the RangeError is built in (a quarter of a small stack).
uintptr_t rtStackLimitFor(uintptr_t low, uintptr_t high) noexcept;
// Arms an interrupt on the thread whose block `tls` is. Any thread; a store.
void rtArmInterrupt(bronze_tls_block* tls) noexcept;
// Puts the real limit back over an armed one, on the calling thread.
void rtDisarmInterrupt() noexcept;
// What an armed interrupt runs, on the interrupted thread, once the limit is
// back: the compartment scheduler's check (compartment.cpp), which may
// suspend the running fiber or throw a TerminationException.
void rtServiceInterrupt();

}  // namespace bronze::runtime
