// Per-thread instances of a compiled module's mutable data (bronze_abi.h,
// `bronze_module_instance`).
//
// A module image's writable tables — its inline caches, provided-global cache,
// template cells, environment cell and native import table — hold Values and
// heap-derived addresses, and a Value is only meaningful on the thread whose
// heap it points into. Before this seam those tables were per IMAGE, so one
// image entered on two threads had both threads latching their own heap's
// objects into one IC table and both collectors forwarding the same cells:
// a worker running a module the main thread had already run would jump into
// a function object that lived — or had lived — in the main thread's heap.
//
// So the tables are per THREAD. The backend lays them out as one contiguous
// run of the image's .data, [`__bronze_instance`, `__bronze_instance_end`),
// and addresses every one of them as `image address + delta`, where delta is
// the calling thread's offset from the image's run to its own copy. The
// first thread to enter a module (its home) uses the image's run itself, so
// delta is 0 and a single-threaded program's addresses are exactly what they
// always were; every later thread gets a heap copy of the run as it stood
// BEFORE the home thread wrote anything into it, which is the snapshot taken
// at first registration.
//
// Generated code reads the delta without a call: the image's slot cell
// (outside the run, process-wide, written once under the mutex below) names
// an index into the thread's delta array, which the TLS block publishes as
// `module_deltas`. A delta never changes once written, and a grown array
// never frees its predecessor, so a function that loaded the array pointer
// before another module registered still reads its own module's delta
// correctly through the old array.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/fatal.h"
#include "runtime/tls_block.h"

namespace bronze::runtime {

namespace {

// One registered image: where its run lives and what the run held before its
// home thread touched it. Indexed by slot - 1; slot 0 is "never registered".
struct ImageRecord {
    const uint8_t* begin;
    size_t size;
    std::vector<uint8_t> pristine;
};

std::mutex& imagesMutex() {
    static auto* m = new std::mutex();
    return *m;
}

std::deque<ImageRecord>& images() {
    static auto* v = new std::deque<ImageRecord>();
    return *v;
}

// The marker for "this thread has no instance of that slot". A real delta is
// the difference of two user-space addresses and can never be all-ones.
constexpr uint64_t kNoDelta = ~uint64_t{0};

// The copies are allocated at this alignment, and the image's run starts at
// it (brass_backend_sections.cpp), so every table in a copy keeps the
// alignment the backend gave it in the image.
constexpr size_t kInstanceAlign = 64;

thread_local uint64_t* t_deltas = nullptr;
thread_local size_t t_deltaCapacity = 0;

void ensureDeltaCapacity(size_t slot) {
    if (slot < t_deltaCapacity) return;
    size_t cap = t_deltaCapacity == 0 ? 16 : t_deltaCapacity;
    while (cap <= slot) cap *= 2;
    auto* grown = new uint64_t[cap];
    for (size_t i = 0; i < cap; ++i) grown[i] = kNoDelta;
    if (t_deltas) std::memcpy(grown, t_deltas, t_deltaCapacity * sizeof(uint64_t));
    // The old array is leaked on purpose: generated code may hold its address
    // across the call that grew it (see the header comment).
    t_deltas = grown;
    t_deltaCapacity = cap;
    rtTls()->module_deltas = grown;
}

}  // namespace

}  // namespace bronze::runtime

extern "C" uint64_t bronze_module_instance(uint64_t* slotCell, uint64_t* begin, uint64_t* end) {
    using namespace bronze::runtime;
    if (!slotCell || !begin || !end || end < begin) {
        bronze::fatal("bronze_module_instance: a module entry passed no instance run");
    }
    std::atomic_ref<uint64_t> slotRef(*slotCell);
    uint64_t slot = slotRef.load(std::memory_order_acquire);
    if (slot != 0 && slot < t_deltaCapacity && t_deltas[slot] != kNoDelta) {
        return t_deltas[slot];
    }

    const auto* runBegin = reinterpret_cast<const uint8_t*>(begin);
    const size_t size = static_cast<size_t>(reinterpret_cast<const uint8_t*>(end) - runBegin);
    uint64_t delta = 0;
    {
        std::lock_guard<std::mutex> lock(imagesMutex());
        slot = slotRef.load(std::memory_order_relaxed);
        if (slot == 0) {
            // The home thread: the image's own run is its instance, and the
            // run as it stands now — before this entry writes anything — is
            // what every later thread starts from.
            images().push_back({runBegin, size, std::vector<uint8_t>(runBegin, runBegin + size)});
            slot = images().size();
            slotRef.store(slot, std::memory_order_release);
        } else {
            const ImageRecord& rec = images()[slot - 1];
            if (rec.begin != runBegin || rec.size != size) {
                bronze::fatal("bronze_module_instance: a module's slot cell names another image");
            }
            void* copy = ::operator new(size == 0 ? 1 : size, std::align_val_t{kInstanceAlign});
            if (size != 0) std::memcpy(copy, rec.pristine.data(), size);
            delta = reinterpret_cast<uint64_t>(copy) - reinterpret_cast<uint64_t>(runBegin);
        }
    }
    ensureDeltaCapacity(static_cast<size_t>(slot));
    t_deltas[slot] = delta;
    return delta;
}
