// The microtask QUEUE and the unhandled-rejection registry: order, re-entrancy
// and roots. What a job means is promise_test.cpp's subject; what is pinned
// here is that jobs run in the order they were enqueued, that a job may enqueue
// another into the same drain, and that a job's captures survive a collection
// while it is sitting in the queue — which is the one root path nothing on any
// stack provides.
//
// The queue and the registry are process-global, so every case here drains to
// quiescence before it finishes. A test that left a job behind would run it
// inside the next test's drain and be very hard to read afterwards.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/microtask.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// Every case leaves the queue and the registry empty, whatever it asserted.
struct DrainGuard {
    ~DrainGuard() { rtDrainMicrotasks(); }
};

// What the handlers below record, in the order they ran. A C++ vector and not
// a JS array: the point of the test is the ORDER, and a heap array would make
// the observation itself an allocation.
std::vector<std::string> g_log;

// A handler that appends the string it was built for. Its env is the tag, as a
// heap string — which is also what makes it a capture the collector has to
// keep alive while the job waits in the queue.
uint64_t logHandler(uint64_t env, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> tag{Value(env)};
    g_log.push_back(rtUtf8Chars(tag.get().asString<StringHeader>()));
    return Value::fromUndefined().rawBits();
}

// The same, plus one more job enqueued from inside — 9.5's "a job may enqueue
// jobs, and they run in the same checkpoint".
uint64_t logAndEnqueue(uint64_t env, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> tag{Value(env)};
    g_log.push_back(rtUtf8Chars(tag.get().asString<StringHeader>()));
    Rooted<Value> nested{rtMakeString("nested")};
    Rooted<Value> handler{rtMakeNativeClosure(logHandler, nested, 1)};
    rtEnqueueReactionJob(handler.get(), Value::fromUndefined(), Value::fromUndefined(),
                         /*rejected=*/false);
    return Value::fromUndefined().rawBits();
}

Value makeLogger(const char* tag, NativeFunctionCode code = logHandler) {
    Rooted<Value> name{rtMakeString(tag)};
    return rtMakeNativeClosure(code, name, 1);
}

}  // namespace

TEST_CASE("the queue is FIFO and a job may enqueue into the same drain") {
    ShadowStackFrame frame;
    DrainGuard guard;
    g_log.clear();

    Rooted<Value> first{makeLogger("first")};
    Rooted<Value> second{makeLogger("second", logAndEnqueue)};
    Rooted<Value> third{makeLogger("third")};
    Rooted<Value> noCap{Value::fromUndefined()};
    Rooted<Value> arg{Value::fromUndefined()};
    rtEnqueueReactionJob(first.get(), noCap.get(), arg.get(), false);
    rtEnqueueReactionJob(second.get(), noCap.get(), arg.get(), false);
    rtEnqueueReactionJob(third.get(), noCap.get(), arg.get(), false);
    CHECK(rtMicrotaskQueueLength() == 3);
    CHECK(rtMicrotasksPending());

    rtDrainMicrotasks();

    // The nested job runs LAST, not immediately after the job that queued it:
    // a drain is a queue, not a stack.
    REQUIRE(g_log.size() == 4);
    CHECK(g_log[0] == "first");
    CHECK(g_log[1] == "second");
    CHECK(g_log[2] == "third");
    CHECK(g_log[3] == "nested");
    CHECK_FALSE(rtMicrotasksPending());
}

TEST_CASE("a queued job's captures survive a forced collection") {
    // The queue is the collector's ONLY view of these: the handler was built
    // in this frame, but the argument below is dropped from every root the
    // moment it is enqueued. microtask.cpp registers the queue as a root
    // source for exactly this, and the failure without it is silent — a
    // forwarded-from-space string read as characters.
    ShadowStackFrame frame;
    DrainGuard guard;
    g_log.clear();

    {
        Rooted<Value> handler{makeLogger("survivor")};
        Rooted<Value> noCap{Value::fromUndefined()};
        Rooted<Value> arg{rtMakeString("argument")};
        rtEnqueueReactionJob(handler.get(), noCap.get(), arg.get(), false);
    }
    for (int i = 0; i < 32; ++i) {
        Rooted<Value> garbage{rtMakeString("junk")};
        (void)garbage;
        rtHeap().collect();
    }

    rtDrainMicrotasks();
    REQUIRE(g_log.size() == 1);
    CHECK(g_log[0] == "survivor");
}

TEST_CASE("a rejection with no handler parks, and a later subscription unparks it") {
    ShadowStackFrame frame;
    DrainGuard guard;
    rtDrainMicrotasks();  // clear whatever a previous case parked
    CHECK(rtParkedRejectionCount() == 0);

    Rooted<Value> promise{rtNewPromise()};
    Rooted<Value> reason{rtMakeString("nobody is listening")};
    rtRejectPromise(promise, reason);
    CHECK(rtPromiseStateOf(promise.get()) == PromiseState::Rejected);
    CHECK(rtParkedRejectionCount() == 1);

    // Subscribing HANDLES it (27.2.5.4.1 step 10), which is what lets a
    // `catch` attached in the same tick as the rejection cancel the report.
    Rooted<Value> onF{Value::fromUndefined()};
    Rooted<Value> onR{makeLogger("caught")};
    Rooted<Value> cap{Value::fromUndefined()};
    rtPerformPromiseThen(promise, onF, onR, cap);
    CHECK(rtParkedRejectionCount() == 0);
}

TEST_CASE("the drain reports a still-parked rejection once and empties the registry") {
    ShadowStackFrame frame;
    DrainGuard guard;
    rtDrainMicrotasks();
    CHECK(rtParkedRejectionCount() == 0);

    Rooted<Value> promise{rtNewPromise()};
    Rooted<Value> reason{rtMakeString("dropped")};
    rtRejectPromise(promise, reason);
    CHECK(rtParkedRejectionCount() == 1);

    // The report itself goes to stderr; what is pinned here is that it is made
    // ONCE — a second drain (an embedder calling in again) must not repeat it.
    rtDrainMicrotasks();
    CHECK(rtParkedRejectionCount() == 0);
    rtDrainMicrotasks();
    CHECK(rtParkedRejectionCount() == 0);
}

TEST_CASE("a parked promise survives collections until the drain reports it") {
    ShadowStackFrame frame;
    DrainGuard guard;
    rtDrainMicrotasks();

    {
        Rooted<Value> promise{rtNewPromise()};
        Rooted<Value> reason{rtMakeString("unrooted but parked")};
        rtRejectPromise(promise, reason);
    }
    CHECK(rtParkedRejectionCount() == 1);
    for (int i = 0; i < 16; ++i) {
        Rooted<Value> garbage{rtMakeString("junk")};
        (void)garbage;
        rtHeap().collect();
    }
    // The registry is a root source too, so the report can still read the
    // reason out of a promise no frame holds.
    CHECK(rtParkedRejectionCount() == 1);
    rtDrainMicrotasks();
    CHECK(rtParkedRejectionCount() == 0);
}
