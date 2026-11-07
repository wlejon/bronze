// Host interrupts (runtime/host_interrupt.h): a function the host has a
// thread run at its next poll, requested from another thread while that
// thread runs a long stretch of script. Each case runs on a thread of its own.
#include <doctest/doctest.h>

#include "embed/embed.h"
#include "eval/eval.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_state.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

using namespace bronze;
using namespace bronze::eval;

namespace {

void onThread(const std::function<void()>& body) {
    std::thread worker([&] {
        ShadowStackFrame frame;
        body();
    });
    worker.join();
}

struct Hits {
    std::atomic<int> count{0};
    std::thread::id thread;
    bool otherThread = false;
};

void onInterrupt(void* arg) {
    auto* h = static_cast<Hits*>(arg);
    if (std::this_thread::get_id() != h->thread) h->otherThread = true;
    h->count.fetch_add(1, std::memory_order_relaxed);
}

const ExecutionTier kTiers[] = {ExecutionTier::Tier0_Interpreter, ExecutionTier::Tier1_Baseline,
                                ExecutionTier::Tier2_Optimized, ExecutionTier::Auto};

}  // namespace

TEST_CASE("host interrupt: a busy script runs the host's function on its own thread, in every tier") {
    for (ExecutionTier tier : kTiers) {
        CAPTURE(static_cast<int>(tier));
        onThread([&] {
            Hits hits;
            hits.thread = std::this_thread::get_id();
            embed::HostInterrupt* h = embed::registerHostInterrupt(&onInterrupt, &hits);
            REQUIRE(h != nullptr);
            std::atomic<bool> done{false};
            std::thread requester([&] {
                while (!done.load()) {
                    embed::requestHostInterrupt(h);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });
            EvalOptions opts;
            opts.filename = "<host-interrupt>";
            opts.tier = tier;
            embed::CallResult r = evalScript(
                "const end = Date.now() + 200; let n = 0; while (Date.now() < end) { n++; } globalThis.n = n;",
                opts);
            done.store(true);
            requester.join();
            CHECK_FALSE(r.thrown);
            MESSAGE("tier ", static_cast<int>(tier), ": ", hits.count.load(), " host interrupts in 200 ms");
            CHECK(hits.count.load() >= 5);
            CHECK_FALSE(hits.otherThread);
            // Unregistered: a request runs nothing.
            embed::unregisterHostInterrupt();
            const int before = hits.count.load();
            embed::requestHostInterrupt(h);
            r = evalScript("let m = 0; for (let i = 0; i < 100000; i++) m += i; globalThis.m = m;", opts);
            CHECK_FALSE(r.thrown);
            CHECK(hits.count.load() == before);
        });
    }
}
