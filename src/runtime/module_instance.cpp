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
//
// PER REALM, not only per thread. A module's environment cell is in the run,
// so two realms on one thread running the same compiled code would share its
// top-level bindings (and its inline caches would latch one realm's objects
// for the other) if they shared an instance. Each realm therefore has a table
// of its own, and "thread" above reads "(thread, realm)": the first realm in
// the process to enter a module uses the image's run, every other gets a
// pristine copy. realm.cpp activates the current realm's table on every
// enter and exit, which is a pointer store into the TLS block; the default
// realm keeps the thread's own table, so a host with one realm sees nothing
// change.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/module_instance.h"
#include "runtime/atomic_ref.h"
#include "runtime/fatal.h"
#include "runtime/rt_state.h"
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

// One realm's instances on this thread (the thread's own table serves its
// default realm).
struct ThreadModuleInstanceState {
    uint64_t* deltas = nullptr;
    size_t deltaCapacity = 0;
    std::vector<uint64_t*> retiredDeltas;
    std::vector<void*> threadCopies;
    // Where each instance this table uses lives (the image's run for the
    // home, else a copy): what a destroyed realm's GC registrations point at.
    std::vector<std::pair<const uint8_t*, size_t>> runs;

    void clear() {
        delete[] deltas;
        deltas = nullptr;
        deltaCapacity = 0;
        for (uint64_t* old : retiredDeltas) {
            delete[] old;
        }
        retiredDeltas.clear();
        retiredDeltas.shrink_to_fit();
        for (void* copy : threadCopies) {
            ::operator delete(copy, std::align_val_t{kInstanceAlign});
        }
        threadCopies.clear();
        threadCopies.shrink_to_fit();
        runs.clear();
        runs.shrink_to_fit();
    }

    ~ThreadModuleInstanceState() { clear(); }
};

thread_local ThreadModuleInstanceState t_inst;
// The other realms' tables, by realm slot, and the one generated code reads
// now. Heap-allocated so a table's address is stable while the map grows.
thread_local std::unordered_map<uint32_t, std::unique_ptr<ThreadModuleInstanceState>> t_realmInst;
thread_local ThreadModuleInstanceState* t_active = nullptr;

ThreadModuleInstanceState& active() { return t_active ? *t_active : t_inst; }

void ensureDeltaCapacity(size_t slot) {
    ThreadModuleInstanceState& inst = active();
    if (slot < inst.deltaCapacity) return;
    size_t cap = inst.deltaCapacity == 0 ? 16 : inst.deltaCapacity;
    while (cap <= slot) cap *= 2;
    auto* grown = new uint64_t[cap];
    for (size_t i = 0; i < cap; ++i) grown[i] = kNoDelta;
    if (inst.deltas) {
        std::memcpy(grown, inst.deltas, inst.deltaCapacity * sizeof(uint64_t));
        // Kept alive for the life of the table: generated code may hold its
        // address across the call that grew it (see the header comment).
        inst.retiredDeltas.push_back(inst.deltas);
    }
    inst.deltas = grown;
    inst.deltaCapacity = cap;
    rtTls()->module_deltas = grown;
}

}  // namespace

bool rtThreadHasModuleInstance(const uint64_t* slotCell) {
    if (!slotCell) return false;
    AtomicRef<uint64_t> slotRef(*const_cast<uint64_t*>(slotCell));
    const uint64_t slot = slotRef.load(std::memory_order_acquire);
    const ThreadModuleInstanceState& inst = active();
    return slot != 0 && slot < inst.deltaCapacity && inst.deltas[slot] != kNoDelta;
}

void rtActivateRealmModuleInstances(uint32_t realmSlot) {
    ThreadModuleInstanceState* next = &t_inst;
    if (realmSlot != 0) {
        auto& slotTable = t_realmInst[realmSlot];
        if (!slotTable) slotTable = std::make_unique<ThreadModuleInstanceState>();
        next = slotTable.get();
    }
    if (next == &active() && rtTls()->module_deltas == next->deltas) return;
    t_active = next;
    rtTls()->module_deltas = next->deltas;
}

void rtReleaseRealmModuleInstances(uint32_t realmSlot) {
    if (realmSlot == 0) return;
    auto it = t_realmInst.find(realmSlot);
    if (it != t_realmInst.end()) {
        if (it->second.get() == t_active) {
            t_active = &t_inst;
            rtTls()->module_deltas = t_inst.deltas;
        }
        for (const auto& [addr, size] : it->second->runs) rtDropModuleCellsWithin(addr, size);
        t_realmInst.erase(it);
    }
    rtDropRealmFunctionSingletons(realmSlot);
}

void rtTeardownThreadModuleInstances() {
    rtTls()->module_deltas = nullptr;
    t_active = nullptr;
    t_realmInst.clear();
    t_inst.clear();
}

}  // namespace bronze::runtime

extern "C" uint64_t bronze_module_instance(uint64_t* slotCell, uint64_t* begin, uint64_t* end) {
    using namespace bronze::runtime;
    if (!slotCell || !begin || !end || end < begin) {
        bronze::fatal("bronze_module_instance: a module entry passed no instance run");
    }
    AtomicRef<uint64_t> slotRef(*slotCell);
    uint64_t slot = slotRef.load(std::memory_order_acquire);
    ThreadModuleInstanceState& inst = active();
    if (slot != 0 && slot < inst.deltaCapacity && inst.deltas[slot] != kNoDelta) {
        return inst.deltas[slot];
    }

    const auto* runBegin = reinterpret_cast<const uint8_t*>(begin);
    const size_t size = static_cast<size_t>(reinterpret_cast<const uint8_t*>(end) - runBegin);
    uint64_t delta = 0;
    void* newCopy = nullptr;
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
            newCopy = ::operator new(size == 0 ? 1 : size, std::align_val_t{kInstanceAlign});
            if (size != 0) std::memcpy(newCopy, rec.pristine.data(), size);
            delta = reinterpret_cast<uint64_t>(newCopy) - reinterpret_cast<uint64_t>(runBegin);
        }
    }
    if (newCopy) {
        inst.threadCopies.push_back(newCopy);
    }
    inst.runs.emplace_back(
        reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(runBegin) + delta), size);
    ensureDeltaCapacity(static_cast<size_t>(slot));
    inst.deltas[slot] = delta;
    return delta;
}
