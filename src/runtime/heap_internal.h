#pragma once

// The two things more than one of the heap's translation units has to know.
// The heap is one class split across three files by what a pass DOES to the
// space: heap.cpp reserves and hands out memory, heap_collect.cpp copies the
// live set out of it, heap_verify.cpp walks it afterwards and audits it. What
// crosses those seams is a tag predicate the collector and the verifier must
// answer identically, and one process-wide counter block the allocation path
// and the collector both add to.

#include <chrono>
#include <cstdint>

#include "runtime/value.h"

namespace bronze {
namespace heap_internal {

// Measurement, not policy: BRONZE_GC_LOG=1 prints at exit how much of a run
// the collector actually was — collections, bytes copied vs bytes allocated,
// and wall time inside collect(). It exists because "allocation-heavy loop"
// names two different bills (copying survivors, and the per-object allocation
// path itself) and only a number says which one a benchmark is paying.
struct GcLogStats {
    bool enabled{false};
    uint64_t alloc_bytes{0};
    uint64_t alloc_count{0};
    uint64_t collections{0};
    uint64_t copied_bytes{0};
    uint64_t gc_nanos{0};
    std::chrono::steady_clock::time_point start;
};

// Defined in heap.cpp, beside the Heap constructor that reads the environment
// variable and registers the atexit dump: the allocation path adds to it there
// and the collector adds to it in heap_collect.cpp.
extern GcLogStats g_gcLog;

void dumpGcLog();

// Does this object's payload hold Values the collector must trace? No for the
// three whose payload is raw bytes — a string's code units, an ArrayBuffer's
// storage, a BigInt's limbs — and reading any of them as Values would forward
// whatever bit pattern happened to look like a heap pointer.
//
// The copy phase and the verify pass ask this of the same headers, and an
// answer they disagreed on would be a word one of them traces and the other
// refuses to parse — so it is one definition, not two.
inline bool payload_holds_values(uint16_t tag) noexcept {
    return tag != static_cast<uint16_t>(Tag::String) &&
           tag != static_cast<uint16_t>(Tag::RawBytes) &&
           tag != static_cast<uint16_t>(Tag::BigInt);
}

}  // namespace heap_internal

using namespace heap_internal;

}  // namespace bronze
