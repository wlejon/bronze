// The microtask queue and the unhandled-rejection registry. What a job DOES
// lives in promise.cpp (the two job bodies are promise semantics); what is
// here is the order they run in, the roots that keep their captures alive,
// and the report for a rejection nothing ever handled.

#include "runtime/microtask.h"

#include <cstdio>
#include <deque>
#include <vector>

#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/promise.h"
#include "runtime/rt_state.h"
#include "runtime/weak_ref.h"

namespace bronze::runtime {

namespace {

// One queued job: a kind and up to three captured Values. A flat struct
// rather than a closure so the root source below can walk every capture
// without knowing which kind it is looking at.
struct Job {
    enum Kind : uint32_t { ReactionFulfill, ReactionReject, Thenable };
    uint32_t kind;
    Value v0;  // reaction: handler      thenable: the promise being resolved
    Value v1;  // reaction: capability   thenable: the thenable
    Value v2;  // reaction: argument     thenable: its `then` method
};

// A deque, because the two operations are push-back and pop-front and both
// pointers and iterators into it are never held across a mutation — the drain
// copies a job out before popping it.
std::deque<Job> g_queue;

// Rejected promises with no handler yet, in rejection order — which is the
// order they are reported in, so the report is deterministic. A vector and a
// linear scan: parking is rare (it is the error path) and the registry is
// almost always empty or one deep.
std::vector<Value> g_parked;

// Registered on FIRST USE, not at static initialization, for the reason
// exception.cpp's ensureExceptionRoots records: rtHeap() lives in another
// translation unit, and registering into it from a static initializer here
// is the initialization-order fiasco.
//
// The queue and the registry are the collector's ONLY view of an enqueued
// job's captures: a reaction enqueued by an already-settled promise has left
// the promise's own reaction lists, and nothing on any frame holds it.
void ensureQueueRoots() {
    static const bool registered = [] {
        rtHeap().add_root_source([](const Heap::RootVisitor& visit) {
            for (Job& job : g_queue) {
                visit(job.v0);
                visit(job.v1);
                visit(job.v2);
            }
            for (Value& promise : g_parked) visit(promise);
        });
        return true;
    }();
    (void)registered;
}

// The report for one promise nothing handled. Stderr, so an oracle case can
// still pin stdout byte-for-byte around it; `Uncaught` is not borrowed
// because this is a different fact — the value was never thrown, it is a
// rejection the program ignored.
void reportParkedRejections() {
    if (g_parked.empty()) return;
    std::fflush(stdout);
    for (const Value& promise : g_parked) {
        const std::string text = rtPromiseRejectionText(promise);
        std::fprintf(stderr, "Unhandled promise rejection: %s\n", text.c_str());
    }
    std::fflush(stderr);
    // Reported ONCE: a second drain (an embedder calling in again) must not
    // repeat what has already been said about the same promise.
    g_parked.clear();
    // The exit code stays 0, deliberately. node made the same report fatal in
    // v15, but bronze's drain runs once at program end — there is no later
    // turn the program could have attached a handler on, so a rejection that
    // reaches here is far more often a fire-and-forget async call than a
    // lost error. The report is the loud part; the exit code stays the
    // program's own, so a program that printed its output and ignored a
    // promise still exits the way it observably behaved.
}

}  // namespace

void rtEnqueueReactionJob(Value handler, Value capability, Value argument, bool rejected) {
    ensureQueueRoots();
    g_queue.push_back(Job{rejected ? Job::ReactionReject : Job::ReactionFulfill, handler,
                          capability, argument});
}

void rtEnqueueThenableJob(Value promise, Value thenable, Value thenFn) {
    ensureQueueRoots();
    g_queue.push_back(Job{Job::Thenable, promise, thenable, thenFn});
}

bool rtMicrotasksPending() { return !g_queue.empty(); }

void rtDrainMicrotasks() {
    // The synchronous half of the program is itself a job, and it has just
    // ended: 9.13 ClearKeptObjects runs "when no ECMAScript code is running",
    // so whatever a `WeakRef.prototype.deref` in the main script pinned is
    // released HERE and can be collected from now on. Doing it at the top of
    // the drain rather than at the bottom is what lets a cleanup callback fire
    // in the same drain for an object the main script deref'd.
    rtClearKeptObjects();
    while (!g_queue.empty() || rtFinalizationCleanupPending()) {
        // A FinalizationRegistry cleanup job is a HOST job (26.2.1.2's caller is
        // the host, not the language), so it runs at a checkpoint the microtask
        // queue has already reached quiescence at — a cleanup callback must
        // never be interleaved with a promise reaction the program was already
        // waiting on. It may enqueue microtasks of its own, which the outer loop
        // then drains before the next batch of cleanups.
        if (g_queue.empty()) {
            rtRunFinalizationCleanupJob();
            rtClearKeptObjects();
            continue;
        }
        // The in-flight job's captures move from the queue (rooted by the
        // source above) into these frame roots BEFORE the pop — at no point
        // is the job's data outside the collector's view, and the job body
        // may allocate freely.
        Rooted<Value> v0{g_queue.front().v0};
        Rooted<Value> v1{g_queue.front().v1};
        Rooted<Value> v2{g_queue.front().v2};
        const uint32_t kind = g_queue.front().kind;
        g_queue.pop_front();

        switch (kind) {
            case Job::ReactionFulfill:
                rtRunReactionJob(v0, v1, v2, /*rejected=*/false);
                break;
            case Job::ReactionReject:
                rtRunReactionJob(v0, v1, v2, /*rejected=*/true);
                break;
            case Job::Thenable:
                rtRunThenableJob(v0, v1, v2);
                break;
            default:
                fatal("internal: an unknown microtask kind in the queue");
        }
        // Every job body settles a promise with what its callback threw
        // rather than letting it escape (that IS 27.2.2.1 step 1.e-1.g). An
        // exception still pending here is a runtime that lost its unwind,
        // not a program error — same rule as rtThrow's double-raise check.
        if (rtExceptionPending()) {
            fatal("internal: an exception escaped a microtask job");
        }
        // Between jobs is the other "no ECMAScript code is running" point, so
        // a target a reaction handler deref'd stops being kept here rather than
        // at the end of the whole drain.
        rtClearKeptObjects();
    }
    reportParkedRejections();
}

void rtParkRejection(Value promise) {
    ensureQueueRoots();
    for (const Value& parked : g_parked) {
        if (parked.rawBits() == promise.rawBits()) return;
    }
    g_parked.push_back(promise);
}

void rtUnparkRejection(Value promise) {
    for (size_t i = 0; i < g_parked.size(); ++i) {
        if (g_parked[i].rawBits() == promise.rawBits()) {
            g_parked.erase(g_parked.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

size_t rtMicrotaskQueueLength() { return g_queue.size(); }
size_t rtParkedRejectionCount() { return g_parked.size(); }

}  // namespace bronze::runtime
