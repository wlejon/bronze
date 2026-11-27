#pragma once

// Process-wide wait/notify table and thread-local async-waiter registry for
// Atomics.wait / Atomics.notify / Atomics.waitAsync (ECMA-262 25.4).

#include <atomic>
#include <cstdint>

#include "runtime/value.h"

namespace bronze::runtime {

// ECMA-262 25.4.3.13 Atomics.wait(typedArray, index, value, timeout)
Value rtAtomicsWait(Value viewVal, Value indexVal, Value expectedVal, Value timeoutVal);

// ECMA-262 25.4.3.8 Atomics.notify(typedArray, index, count)
Value rtAtomicsNotify(Value viewVal, Value indexVal, Value countVal);

// ECMA-262 25.4.3.15 Atomics.waitAsync(typedArray, index, value, timeout)
Value rtAtomicsWaitAsync(Value viewVal, Value indexVal, Value expectedVal, Value timeoutVal);

// Settles any async waiters on the current thread that have been notified or
// whose finite deadline has elapsed, enqueuing their promise reactions.
void rtDrainAsyncAtomicsWaiters();

// Lock-free check whether any async waiter on the current thread is already
// ready to settle (notified or past deadline) or already in the settling list.
bool rtHasReadyAsyncAtomicsWaiters() noexcept;

// Detaches/drops any still-pending async waiters owned by the exiting thread.
void rtClearThreadAsyncAtomicsWaiters() noexcept;

// Helpers for aligned/unaligned atomic accesses in builtin_shared_memory.cpp.
uint64_t rtAtomicLoadBytes(const uint8_t* ptr, uint32_t width) noexcept;
void rtAtomicStoreBytes(uint8_t* ptr, uint32_t width, uint64_t value) noexcept;
uint64_t rtAtomicRmwBytes(uint8_t* ptr, uint32_t width, int op, uint64_t operand) noexcept;
uint64_t rtAtomicCompareExchangeBytes(uint8_t* ptr, uint32_t width, uint64_t expected,
                                      uint64_t replacement) noexcept;

}  // namespace bronze::runtime
