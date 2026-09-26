// The promise core, below the compiler.
//
// The oracle cases pin what a PROGRAM can see — the order lines print in, what
// a combinator resolves with, how an await interleaves with a `.then` chain
// (async_await_single_tick). Two things they cannot see are pinned here
// instead, because each is a decision rather than an observation:
//
//   - PromiseResolve's intrinsic fast path (27.2.4.7 step 2), which is where
//     an await's single tick is saved.
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

TEST_CASE("a pending promise's reactions survive a collection") {
    // The subscribed handlers live in the promise's own reaction lists, which
    // are ordinary heap arrays hanging off internal slots — so this is really
    // asking whether an internal slot is traced. It is the same question a
    // suspended async body asks one link further out (its owner is the
    // environment of the reactions its await subscribed).
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
    // 27.2.1.3.2 step 7 is a rejection, so it is parked like any other.
    CHECK(rtParkedRejectionCount() == 1);
}
