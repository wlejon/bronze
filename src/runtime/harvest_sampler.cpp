// The harvest's samples (harvest_sampler.h).

#include "runtime/harvest_sampler.h"

#include <atomic>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "runtime/stack_trace.h"
#include "runtime/tls_block.h"

namespace bronze::runtime {

std::atomic<bronze_tls_block*> g_harvestSampleWanted{nullptr};

namespace {

// Leaked on purpose: a sampler thread and the atexit writer may outlive
// every static.
struct Sampler {
    std::mutex mu;  // everything below
    std::condition_variable wake;
    bronze_tls_block* tls = nullptr;  // the sampled thread's block
    bool live = false;     // the sampled thread has not exited
    bool running = false;  // a sampler thread of generation `gen` runs
    uint64_t gen = 0;      // bumped by every start and stop
    uint64_t taken = 0;    // samples taken in this process
    uint32_t passed = 0;   // polls in a row that passed the sample on
    bool parked = false;   // the sampler waits, untimed, for the next take
    std::unordered_map<const bronze_code_range*, uint64_t> counts;
};

Sampler* g_sampler = nullptr;
std::once_flag g_once;

Sampler& sampler() {
    std::call_once(g_once, [] { g_sampler = new Sampler(); });
    return *g_sampler;
}

// On a sampled thread: its exit ends the sampling of it (its block goes with
// it, and nothing may arm it after).
struct TargetGuard {
    bronze_tls_block* tls = nullptr;
    ~TargetGuard() {
        if (g_sampler == nullptr || tls == nullptr) return;
        std::lock_guard<std::mutex> lock(g_sampler->mu);
        if (g_sampler->tls != tls) return;
        g_sampler->live = false;
        g_harvestSampleWanted.store(nullptr, std::memory_order_seq_cst);
        g_sampler->wake.notify_all();
    }
};
thread_local TargetGuard t_guard;

// One sampler thread's life: arm, sleep a period, and arm again if the
// sample was taken. If it was not, the JS thread is running no JS (it would
// have polled within microseconds): the sampler parks with no timeout until
// the poll that takes it, which wakes it. So a busy JS thread never makes a
// system call for a sample (the take is an uncontended lock), and an idle
// one costs one period's wakeup, then none.
void run(Sampler* s, uint64_t gen) {
    std::unique_lock<std::mutex> lock(s->mu);
    const auto current = [&] { return s->gen == gen && s->live && s->taken < kSampleLimit; };
    const auto taken = [] { return g_harvestSampleWanted.load(std::memory_order_relaxed) == nullptr; };
    while (current()) {
        // Wanted first, then armed, with a fence between that pairs with the
        // one after every stack-limit store (tls_block.cpp): whichever
        // store comes last leaves the interrupt armed.
        g_harvestSampleWanted.store(s->tls, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        rtArmInterrupt(s->tls);
        s->wake.wait_for(lock, std::chrono::microseconds(kSamplePeriodUs), [&] { return !current(); });
        if (!current()) break;
        if (!taken()) {
            s->parked = true;
            s->wake.wait(lock, [&] { return !current() || taken(); });
            s->parked = false;
        }
    }
    if (s->gen == gen) {
        s->running = false;
        g_harvestSampleWanted.store(nullptr, std::memory_order_seq_cst);
    }
}

}  // namespace

void rtHarvestSamplerStart() {
    Sampler& s = sampler();
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.running || s.taken >= kSampleLimit) return;
    t_guard.tls = rtTls();
    s.tls = t_guard.tls;
    s.live = true;
    s.running = true;
    std::thread(run, &s, ++s.gen).detach();
}

void rtHarvestSamplerStop() {
    if (g_sampler == nullptr) return;
    std::lock_guard<std::mutex> lock(g_sampler->mu);
    ++g_sampler->gen;
    g_sampler->running = false;
    g_harvestSampleWanted.store(nullptr, std::memory_order_seq_cst);
    g_sampler->wake.notify_all();
}

namespace {

// The first function with a descriptor (a JS function, not a call wrapper
// or a stub) among the callers of the frame `fp` names, at most a few frames
// up. Each frame read is one compiled code made (its return address is in a
// code range), so every frame pointer followed is one compiled code set.
const bronze_code_range* callerOf(const void* fp, const bronze_code_range* self) {
    const auto* frame = static_cast<const uintptr_t*>(fp);
    for (int depth = 0; depth < 4 && frame != nullptr; ++depth) {
        if ((reinterpret_cast<uintptr_t>(frame) & 7) != 0) return nullptr;
        const bronze_code_range* range = find_code_range(reinterpret_cast<const void*>(frame[1]));
        if (range == nullptr) return nullptr;
        if (range->desc != nullptr && range != self) return range;
        const auto* next = reinterpret_cast<const uintptr_t*>(frame[0]);
        // Frames are chained toward the stack's base: a link that does not
        // go up, or jumps further than any frame is large, is not one.
        if (next <= frame || reinterpret_cast<uintptr_t>(next) - reinterpret_cast<uintptr_t>(frame) > (1u << 20)) {
            return nullptr;
        }
        frame = next;
    }
    return nullptr;
}

}  // namespace

void rtHarvestSampleAt(const void* pc, const void* fp, bool entry) noexcept {
    bronze_tls_block* tls = rtTls();
    if (g_harvestSampleWanted.load(std::memory_order_relaxed) != tls || g_sampler == nullptr) return;
    Sampler* s = g_sampler;
    const bronze_code_range* range = find_code_range(pc);
    const bronze_code_range* caller = entry && range != nullptr && fp != nullptr ? callerOf(fp, range) : nullptr;
    std::lock_guard<std::mutex> lock(s->mu);
    if (g_harvestSampleWanted.load(std::memory_order_relaxed) != tls) return;
    if (range != nullptr && range->desc == nullptr && s->passed < kSamplePassLimit) {
        // Still wanted: the poll's disarm arms the interrupt again.
        ++s->passed;
        return;
    }
    s->passed = 0;
    ++s->taken;
    if (range != nullptr && range->desc != nullptr) ++s->counts[range];
    if (caller != nullptr) ++s->counts[caller];
    g_harvestSampleWanted.store(nullptr, std::memory_order_seq_cst);
    if (s->parked) s->wake.notify_one();
}

void rtHarvestTakeSamples(const bronze_code_range* ranges, uint32_t count, uint64_t* out) {
    Sampler* s = g_sampler;
    if (s == nullptr || ranges == nullptr) return;
    std::lock_guard<std::mutex> lock(s->mu);
    for (auto it = s->counts.begin(); it != s->counts.end();) {
        if (it->first >= ranges && it->first < ranges + count) {
            out[it->first - ranges] += it->second;
            it = s->counts.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace bronze::runtime
