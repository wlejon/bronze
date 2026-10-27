// Compartments: a realm's entries on a fiber of its own, in time slices
// (compartment.h).

#include "runtime/compartment.h"

#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/microtask.h"
#include "runtime/realm.h"
#include "runtime/stack_trace.h"
#include "runtime/tls_block.h"

#include <brass/gc/fiber.hpp>
#include <brass/runtime/exception.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace bronze::runtime {

namespace {

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

struct Compartment {
    Realm* realm = nullptr;
    CompartmentOptions options;
    int64_t sliceNs = 0;
    std::unique_ptr<brass::gc::Fiber> fiber;
    // The thread's TLS block, for the watchdog's arming store.
    bronze_tls_block* tls = nullptr;
    std::atomic<int64_t> deadlineNs{0};

    CompartmentState state = CompartmentState::Idle;
    CompartmentJob job = nullptr;
    void* jobArg = nullptr;
    bool jobPending = false;   // handed to the fiber, not yet finished
    std::string uncaught;      // the last job's uncaught error

    bool terminateRequested = false;
    std::string terminationReason;
    std::string terminationSite;

    uint32_t overruns = 0;
    double longestOverrunMs = 0.0;
    std::string longestOverrunSite;
};

namespace {

thread_local Compartment* t_running = nullptr;
thread_local int t_noYield = 0;

// ---- the watchdog -----------------------------------------------------------
//
// One thread for the process. It sleeps until the earliest deadline of the
// slices running on any thread, then arms the interrupt of each thread whose
// slice is spent, and again every millisecond while the slice stays spent
// (a stack-limit store of the runtime's own, bronze_tls_enter, can overwrite
// an armed limit; the next tick puts it back).

struct Watchdog {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Compartment*> active;

    Watchdog() {
        std::thread([this] { run(); }).detach();
    }

    void run() {
        std::unique_lock<std::mutex> lock(mu);
        for (;;) {
            if (active.empty()) {
                cv.wait(lock);
                continue;
            }
            const int64_t now = nowNs();
            int64_t wake = INT64_MAX;
            for (Compartment* c : active) {
                const int64_t d = c->deadlineNs.load(std::memory_order_relaxed);
                if (now >= d) {
                    rtArmInterrupt(c->tls);
                    wake = std::min(wake, now + 1'000'000);
                } else {
                    wake = std::min(wake, d);
                }
            }
            cv.wait_for(lock, std::chrono::nanoseconds(wake - now));
        }
    }

    void start(Compartment* c) {
        {
            std::lock_guard<std::mutex> lock(mu);
            c->deadlineNs.store(nowNs() + c->sliceNs, std::memory_order_relaxed);
            active.push_back(c);
        }
        cv.notify_one();
    }

    void stop(Compartment* c) {
        std::lock_guard<std::mutex> lock(mu);
        std::erase(active, c);
    }
};

Watchdog& watchdog() {
    // Never destroyed: its thread runs until the process ends.
    static Watchdog* w = new Watchdog();
    return *w;
}

// Where the running job is, innermost frame first: its stack without the
// "Error" line a stack string starts with.
std::string currentSite() {
    std::string s = bronze_format_stack_trace(Value::fromUndefined());
    const size_t nl = s.find('\n');
    if (nl == std::string::npos) return {};
    s.erase(0, nl + 1);
    // Each frame's leading "    at ".
    std::string out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find('\n', pos);
        if (end == std::string::npos) end = s.size();
        std::string line = s.substr(pos, end - pos);
        const size_t at = line.find("at ");
        if (at != std::string::npos) line.erase(0, at + 3);
        if (!out.empty()) out += '\n';
        out += line;
        pos = end + 1;
    }
    return out;
}

[[noreturn]] void throwTermination(Compartment* c) {
    if (c->terminationSite.empty()) c->terminationSite = currentSite();
    throw brass::runtime::TerminationException(c);
}

// A slice that ended `overNs` ago, measured where the job next reached a
// poll (or returned): past a whole slice, a native held it.
void noteOverrun(Compartment* c, int64_t overNs, bool atPoll) {
    if (overNs <= c->sliceNs) return;
    ++c->overruns;
    const double ms = static_cast<double>(overNs) / 1e6;
    if (ms > c->longestOverrunMs) {
        c->longestOverrunMs = ms;
        c->longestOverrunSite = atPoll ? currentSite() : std::string("(the job's end)");
    }
}

// The fiber's body: one job per resumption from rtCompartmentRun, until the
// compartment is terminated.
void fiberMain(void* arg) {
    auto* c = static_cast<Compartment*>(arg);
    uintptr_t low = 0, high = 0;
    c->fiber->stack_bounds(low, high);
    rtSetStackLimit(rtStackLimitFor(low, high));
    for (;;) {
        if (c->state == CompartmentState::Terminated) return;
        if (c->jobPending) {
            try {
                rtEnterRealm(c->realm);
                {
                    ShadowStackFrame frame;
                    Value caught;
                    if (rtTryCatch([&] { c->job(c->jobArg); }, caught)) {
                        Rooted<Value> thrown{caught};
                        c->uncaught = rtUncaughtText(thrown.get());
                    }
                    // The job's microtask checkpoint, on this stack's queue.
                    rtDrainMicrotasks();
                }
                rtExitRealm();
            } catch (const brass::runtime::TerminationException&) {
                // Its frames are gone; so is the realm entry they made (the
                // fiber's realm stack ends with the fiber).
                c->state = CompartmentState::Terminated;
                c->jobPending = false;
                return;
            } catch (const std::exception& e) {
                // A host error out of the job (an exception leaving the fiber
                // would end the process): reported as the job's, and the
                // compartment ends, since its stack was unwound mid-job.
                c->uncaught = std::string("host error: ") + e.what();
                c->terminationReason = c->uncaught;
                c->state = CompartmentState::Terminated;
                c->jobPending = false;
                return;
            } catch (...) {
                c->uncaught = "host error: an exception of an unknown type left the job";
                c->terminationReason = c->uncaught;
                c->state = CompartmentState::Terminated;
                c->jobPending = false;
                return;
            }
            c->jobPending = false;
        }
        brass::gc::Fiber::suspend();
    }
}

// Runs the fiber until it suspends (a job done, or yielded at a poll) or ends.
void enterFiber(Compartment* c) {
    Compartment* prev = t_running;
    t_running = c;
    c->tls = rtTls();
    watchdog().start(c);
    c->fiber->resume();
    watchdog().stop(c);
    t_running = prev;
    // An interrupt armed for the slice that just ended must not stop the
    // host's own code: the real limit goes back over it.
    rtDisarmInterrupt();
    if (c->state != CompartmentState::Terminated) {
        if (c->jobPending) {
            c->state = CompartmentState::Yielded;
        } else {
            c->state = CompartmentState::Idle;
            noteOverrun(c, nowNs() - c->deadlineNs.load(std::memory_order_relaxed), false);
        }
    }
}

CompartmentStatus statusOf(Compartment* c) {
    CompartmentStatus s;
    s.state = c->state;
    if (c->state == CompartmentState::Idle) s.uncaught = std::move(c->uncaught);
    c->uncaught.clear();
    s.terminationReason = c->terminationReason;
    s.terminationSite = c->terminationSite;
    return s;
}

}  // namespace

void rtServiceInterrupt() {
    Compartment* c = t_running;
    // The host's own stack is never preempted, nor is a compartment's from
    // a stack that is not its fiber.
    if (c == nullptr || brass::gc::Fiber::current() != c->fiber.get()) return;
    if (c->terminateRequested) throwTermination(c);
    const int64_t now = nowNs();
    const int64_t deadline = c->deadlineNs.load(std::memory_order_relaxed);
    if (now < deadline || t_noYield > 0) return;
    noteOverrun(c, now - deadline, true);
    ++c->overruns;
    brass::gc::Fiber::suspend();
    // Resumed (rtCompartmentResume) with a fresh slice, or to be unwound.
    if (c->terminateRequested) throwTermination(c);
}

Compartment* rtCreateCompartment(Realm* realm, const CompartmentOptions& options) {
    auto c = std::make_unique<Compartment>();
    c->realm = realm;
    c->options = options;
    c->sliceNs = static_cast<int64_t>(options.sliceMs * 1e6);
    if (c->sliceNs <= 0) c->sliceNs = 1;
    c->fiber = brass::gc::Fiber::create(&fiberMain, c.get(), options.stackBytes);
    if (!c->fiber) return nullptr;
    return c.release();
}

void rtDestroyCompartment(Compartment* c) {
    if (!c) return;
    rtCompartmentTerminate(c, "the compartment was destroyed");
    // A fiber that started has returned by now (terminate resumed it); one
    // that never ran has nothing on its stack.
    delete c;
}

CompartmentStatus rtCompartmentRun(Compartment* c, CompartmentJob job, void* arg) {
    if (c->state == CompartmentState::Terminated) return statusOf(c);
    if (c->state == CompartmentState::Yielded || c->jobPending) {
        fatal("compartment: a job was run while another one is suspended in it (resume that one first)");
    }
    c->job = job;
    c->jobArg = arg;
    c->jobPending = true;
    enterFiber(c);
    return statusOf(c);
}

CompartmentStatus rtCompartmentResume(Compartment* c) {
    if (c->state != CompartmentState::Yielded) return statusOf(c);
    enterFiber(c);
    return statusOf(c);
}

void rtCompartmentTerminate(Compartment* c, const std::string& reason) {
    if (!c || c->state == CompartmentState::Terminated) return;
    if (!c->terminateRequested) {
        c->terminateRequested = true;
        c->terminationReason = reason;
    }
    if (t_running == c) {
        // From inside one of its jobs: the next poll throws.
        rtArmInterrupt(rtTls());
        return;
    }
    if (!c->fiber->started() || c->fiber->finished()) {
        c->state = CompartmentState::Terminated;
        return;
    }
    if (c->state == CompartmentState::Idle) {
        // Between jobs, suspended in fiberMain's loop: let it return.
        c->state = CompartmentState::Terminated;
        Compartment* prev = t_running;
        t_running = c;
        c->fiber->resume();
        t_running = prev;
        return;
    }
    // Yielded: resumed once, the poll it is suspended in throws, and the
    // job unwinds to fiberMain, which ends.
    enterFiber(c);
}

CompartmentState rtCompartmentState(const Compartment* c) { return c->state; }
Realm* rtCompartmentRealm(const Compartment* c) { return c->realm; }
Compartment* rtRunningCompartment() noexcept { return t_running; }
uint32_t rtCompartmentOverruns(const Compartment* c) { return c->overruns; }
double rtCompartmentLongestOverrunMs(const Compartment* c) { return c->longestOverrunMs; }
const std::string& rtCompartmentLongestOverrunSite(const Compartment* c) { return c->longestOverrunSite; }

void rtBeginNoYield() noexcept { ++t_noYield; }
void rtEndNoYield() noexcept { --t_noYield; }

}  // namespace bronze::runtime
