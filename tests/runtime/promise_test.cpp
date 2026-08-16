// The promise core and the async driver, below the compiler.
//
// The oracle cases pin what a PROGRAM can see — the order lines print in, what
// a combinator resolves with. Three things it cannot see are pinned here
// instead, because each is a decision rather than an observation:
//
//   - the JOB COUNT of an await. `await Promise.resolve(1)` costing one
//     microtask and not three is ES2019's single-tick rule, and the only way
//     to observe it from a program is to interleave it against a `.then`
//     chain — which the oracle case does, and which says nothing about WHERE
//     the tick was saved. Here it is counted directly.
//   - the ROOT PATH of a suspended async frame. Nothing on any stack holds
//     one; it hangs off the reaction closures, which hang off either the
//     awaited promise or the job queue. A forced collection between the
//     suspension and the resumption is the only test that walks it.
//   - the parked/unparked transitions of an unhandled rejection, whose report
//     goes to stderr and so is invisible to a stdout oracle.

#include <doctest/doctest.h>

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/microtask.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

struct DrainGuard {
    ~DrainGuard() { rtDrainMicrotasks(); }
};

// `{ value, done }`, which is what a compiled resume function returns and so
// what the fake body below has to return too.
Value iterResult(Rooted<Value>& value, bool done) {
    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> vk{rtMakeString("value")};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), vk, value);
    Rooted<Value> dk{rtMakeString("done")};
    Rooted<Value> dv{Value::fromBool(done)};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), dk, dv);
    return out.get();
}

// The FRAME of the fake async body: the machine to await through, the value it
// awaits, and where the body is. A compiled body reads all three out of its
// environment record too — this is the same arrangement with three slots
// instead of one per binding.
namespace FrameSlot {
enum : uint32_t { Machine, Awaited, Step, kCount };
}

Value slot(Rooted<Value>& frame, uint32_t index) {
    return frame.get().asObject<ObjectHeader>()->internalSlot(index);
}

void setSlot(Rooted<Value>& frame, uint32_t index, Value v) {
    frame.get().asObject<ObjectHeader>()->setInternalSlot(index, v);
}

// One `await` and nothing else:
//
//     async function f() { return await awaited; }
//
// compiled by hand. Step 0 subscribes and suspends; step 1 is the resumption,
// where mode `throw` re-raises at the suspension point exactly as
// lower_async.cpp's emitted arm does.
uint64_t awaitOnceBody(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> frame{Value(env)};
    Rooted<Value> sent{args[1]};
    const double step = slot(frame, FrameSlot::Step).asNumber();
    if (step == 0) {
        setSlot(frame, FrameSlot::Step, Value::fromDouble(1));
        // The subscription happens BEFORE the suspended result is built, which
        // is the order lower_async.cpp emits and the order the driver trusts.
        Rooted<Value> machine{slot(frame, FrameSlot::Machine)};
        Rooted<Value> awaited{slot(frame, FrameSlot::Awaited)};
        bronze_async_await(machine.get().rawBits(), awaited.get().rawBits());
        Rooted<Value> undef{Value::fromUndefined()};
        return iterResult(undef, /*done=*/false).rawBits();
    }
    if (args[0].asNumber() != 0) {
        // Mode `throw`: raised here, so an enclosing handler in a real body
        // would take it and the driver's "the body threw" arm takes it here.
        return rtThrow(sent.get()).rawBits();
    }
    return iterResult(sent, /*done=*/true).rawBits();
}

// Build the machine over that body and start it, leaving the returned promise
// as the only thing the caller holds — which is the whole point of the
// arrangement: after this returns, the machine is reachable ONLY through the
// awaited promise's reactions.
Value startAwaitOnce(Rooted<Value>& awaited) {
    Rooted<Value> frame{Value::fromUndefined()};
    {
        ObjectHeader* env = ObjectHeader::createWithInternalSlots(
            rtHeap(), rtArena(), rtPlainObjectShape(), FrameSlot::kCount);
        env->header.flags = HeapKind::Plain;
        frame.set(Value::fromObject(env));
    }
    setSlot(frame, FrameSlot::Awaited, awaited.get());
    setSlot(frame, FrameSlot::Step, Value::fromDouble(0));
    Rooted<Value> closure{rtMakeNativeClosure(awaitOnceBody, frame, 2)};
    Rooted<Value> machine{Value(bronze_async_machine(closure.get().rawBits()))};
    // Into the frame BEFORE the start, exactly as lowerAsyncTail emits it.
    setSlot(frame, FrameSlot::Machine, machine.get());
    return Value(bronze_async_start(machine.get().rawBits()));
}

uint64_t noopHandler(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromUndefined().rawBits();
}

Value makeNoop() {
    Rooted<Value> env{Value::fromUndefined()};
    return rtMakeNativeClosure(noopHandler, env, 1);
}

}  // namespace

TEST_CASE("PromiseResolve returns an intrinsic promise unchanged") {
    ShadowStackFrame frame;
    DrainGuard guard;

    Rooted<Value> p{rtNewPromise()};
    // 27.2.4.7 step 2, and the whole of the single-tick rule: the SAME object
    // back, so awaiting one subscribes it directly instead of wrapping it.
    Rooted<Value> same{rtPromiseResolveValue(p)};
    CHECK(same.get().rawBits() == p.get().rawBits());

    // The other arm allocates: a plain value becomes a fresh, already
    // fulfilled promise.
    Rooted<Value> raw{Value::fromDouble(1)};
    Rooted<Value> wrapped{rtPromiseResolveValue(raw)};
    CHECK(wrapped.get().rawBits() != raw.get().rawBits());
    CHECK(rtIsPromise(wrapped.get()));
    CHECK(rtPromiseStateOf(wrapped.get()) == PromiseState::Fulfilled);
    CHECK(rtPromiseResultOf(wrapped.get()).asNumber() == 1);
}

TEST_CASE("an await of an already fulfilled promise costs exactly one job") {
    ShadowStackFrame frame;
    DrainGuard guard;
    rtDrainMicrotasks();
    REQUIRE(rtMicrotaskQueueLength() == 0);

    Rooted<Value> value{Value::fromDouble(1)};
    Rooted<Value> settled{rtPromiseResolveValue(value)};  // `Promise.resolve(1)`
    REQUIRE(rtMicrotaskQueueLength() == 0);
    REQUIRE(rtPromiseStateOf(settled.get()) == PromiseState::Fulfilled);

    Rooted<Value> promise{startAwaitOnce(settled)};
    // ONE. Not two: the fast path above returned the argument unchanged, so no
    // wrapper promise was made and no thenable-adoption job was queued. Not
    // three: that was the pre-ES2019 count this rule removed.
    CHECK(rtMicrotaskQueueLength() == 1);
    CHECK(rtPromiseStateOf(promise.get()) == PromiseState::Pending);

    rtDrainMicrotasks();
    CHECK(rtPromiseStateOf(promise.get()) == PromiseState::Fulfilled);
    CHECK(rtPromiseResultOf(promise.get()).asNumber() == 1);
}

TEST_CASE("a suspended async machine survives collections through the queue") {
    // THE ROOT PATH, walked. After startAwaitOnce returns, nothing in this
    // frame holds the machine, its resume closure or its captured frame: the
    // only edge to them is the reaction closure sitting in `awaited`'s
    // reaction list, and after the settle, in the microtask queue. Both are
    // root sources; without either, this collects the whole suspended body and
    // resumes into freed memory.
    ShadowStackFrame frame;
    DrainGuard guard;

    Rooted<Value> awaited{rtNewPromise()};
    Rooted<Value> promise{startAwaitOnce(awaited)};
    CHECK(rtPromiseStateOf(promise.get()) == PromiseState::Pending);

    for (int i = 0; i < 32; ++i) {
        Rooted<Value> garbage{rtMakeString("junk")};
        (void)garbage;
        rtHeap().collect();
    }

    Rooted<Value> answer{rtMakeString("resumed")};
    rtResolvePromise(awaited, answer);
    // Queued, not run: the resumption is a job like any other.
    CHECK(rtMicrotaskQueueLength() == 1);
    CHECK(rtPromiseStateOf(promise.get()) == PromiseState::Pending);

    // And again with the machine now reachable only through the QUEUE, which
    // is the half of the path the reaction list cannot cover.
    for (int i = 0; i < 32; ++i) {
        Rooted<Value> garbage{rtMakeString("junk")};
        (void)garbage;
        rtHeap().collect();
    }

    rtDrainMicrotasks();
    CHECK(rtPromiseStateOf(promise.get()) == PromiseState::Fulfilled);
    Rooted<Value> result{rtPromiseResultOf(promise.get())};
    CHECK(rtUtf8Chars(result.get().asString<StringHeader>()) == "resumed");
}

TEST_CASE("a rejected await raises at the suspension point and rejects the promise") {
    ShadowStackFrame frame;
    DrainGuard guard;

    Rooted<Value> awaited{rtNewPromise()};
    Rooted<Value> promise{startAwaitOnce(awaited)};
    Rooted<Value> reason{rtNewErrorValue(ErrorKind::TypeError, "awaited badly")};
    rtRejectPromise(awaited, reason);
    // Subscribed by the await, so the awaited promise is HANDLED — only the
    // machine's own promise can end up parked.
    CHECK(rtParkedRejectionCount() == 0);

    rtDrainMicrotasks();
    CHECK(rtPromiseStateOf(promise.get()) == PromiseState::Rejected);
    CHECK(rtIsErrorInstance(rtPromiseResultOf(promise.get())));
    // Nothing subscribed to the machine's promise, so the drain that just ran
    // reported it and cleared the registry.
    CHECK(rtParkedRejectionCount() == 0);
    CHECK_FALSE(rtExceptionPending());
}

TEST_CASE("a pending promise's reactions survive a collection") {
    // The subscribed handlers live in the promise's own reaction lists, which
    // are ordinary heap arrays hanging off internal slots — so this is really
    // asking whether an internal slot is traced. It is the same question the
    // suspended machine asks one link further out, and cheap enough to ask
    // directly.
    ShadowStackFrame frame;
    DrainGuard guard;

    Rooted<Value> promise{rtNewPromise()};
    Rooted<Value> onF{makeNoop()};
    Rooted<Value> onR{Value::fromUndefined()};
    // A capability RECORD, which is what a reaction settles through since
    // `then` began building its result over @@species — the promise is one of
    // its three slots rather than the thing itself.
    Rooted<Value> cap{rtNewPromiseCapabilityForIntrinsic()};
    Rooted<Value> capPromise{rtCapabilityPromise(cap.get())};
    rtPerformPromiseThen(promise, onF, onR, cap);

    for (int i = 0; i < 32; ++i) {
        Rooted<Value> garbage{rtMakeString("junk")};
        (void)garbage;
        rtHeap().collect();
    }

    Rooted<Value> value{Value::fromDouble(7)};
    rtResolvePromise(promise, value);
    rtDrainMicrotasks();
    // The handler answered undefined, and that is what the capability took.
    CHECK(rtPromiseStateOf(capPromise.get()) == PromiseState::Fulfilled);
    CHECK(rtPromiseResultOf(capPromise.get()).isUndefined());
}

TEST_CASE("the first settle wins, whichever half asks") {
    ShadowStackFrame frame;
    DrainGuard guard;

    Rooted<Value> promise{rtNewPromise()};
    Rooted<Value> first{Value::fromDouble(1)};
    Rooted<Value> second{Value::fromDouble(2)};
    rtResolvePromise(promise, first);
    rtResolvePromise(promise, second);
    Rooted<Value> reason{rtMakeString("too late")};
    rtRejectPromise(promise, reason);

    CHECK(rtPromiseStateOf(promise.get()) == PromiseState::Fulfilled);
    CHECK(rtPromiseResultOf(promise.get()).asNumber() == 1);
}

TEST_CASE("resolving a promise with itself rejects rather than throwing") {
    ShadowStackFrame frame;
    DrainGuard guard;

    Rooted<Value> promise{rtNewPromise()};
    Rooted<Value> self{promise.get()};
    rtResolvePromise(promise, self);
    CHECK(rtPromiseStateOf(promise.get()) == PromiseState::Rejected);
    CHECK(rtIsErrorInstance(rtPromiseResultOf(promise.get())));
    CHECK_FALSE(rtExceptionPending());
    // 27.2.1.3.2 step 7 is a rejection, so it is parked like any other.
    CHECK(rtParkedRejectionCount() == 1);
}
