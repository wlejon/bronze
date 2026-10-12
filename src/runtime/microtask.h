#pragma once

#include <cstddef>

#include "runtime/value.h"

// The MICROTASK QUEUE (ECMA-262 9.5, HTML's "perform a microtask checkpoint"
// reduced to what a batch program needs): a FIFO of jobs the promise machinery
// enqueues and a drain the host runs after the program's synchronous half
// finishes. bronze has no event loop — no timers, no IO callbacks — so one
// drain to quiescence at the end of `main` IS the host's loop, and everything
// 9.5 requires of one is that jobs run in order and that a job may enqueue
// more jobs, which run in the same drain.
//
// Also here: the UNHANDLED-REJECTION registry. A promise that rejects with no
// reactions is PARKED; a later `then` (or `catch`, or an `await`) unparks it;
// whatever is still parked when the drain reaches quiescence is reported. The
// registry lives beside the queue because "the drain is over and the queue is
// empty" is the only moment "nothing will ever handle this" is a fact.

namespace bronze::runtime {

// A promise reaction job (27.2.2.1 NewPromiseReactionJob). `handler` may be
// undefined — the identity / thrower defaults, which `rejected` selects
// between. `capability` may be undefined for a reaction that settles nothing
// (an await's subscription). Neither allocates on the bronze heap; the queue
// is C++ memory and a registered GC root source.
void rtEnqueueReactionJob(Value handler, Value capability, Value argument, bool rejected);

// A thenable-adoption job (27.2.2.2 NewPromiseResolveThenableJob): calling
// `thenFn` on `thenable` is deferred to a job so no user `then` runs inside
// the resolve that adopted it.
void rtEnqueueThenableJob(Value promise, Value thenable, Value thenFn);

bool rtMicrotasksPending();

// Run jobs in FIFO order until none remain — jobs enqueued by jobs included —
// then report every still-parked rejection. Idempotent when the queue is
// empty (bar re-reporting nothing).
void rtDrainMicrotasks();

// The rejection registry. Parking the same promise twice and unparking one
// that was never parked are both no-ops, so the callers (promise.cpp's settle
// and subscribe paths) do not have to coordinate.
void rtParkRejection(Value promise);
void rtUnparkRejection(Value promise);

// The embedder's rejection hook (embed.h setPromiseRejectionHook), per thread
// like the rest of the runtime. With one installed, the end-of-drain report
// goes to it instead of stderr — op 0 ("unhandled") per parked promise, which
// also marks the promise Reported — and a later subscription to a Reported
// promise calls it with op 1 ("handled"). `promise` and `reason` are current
// only until the hook's first allocation.
using RejectionHook = void (*)(uint32_t op, Value promise, Value reason);
void rtSetRejectionHook(RejectionHook hook);
// promise.cpp's "handle" half: the promise was Reported and just got its
// first handler. A no-op with no hook installed.
void rtNotifyRejectionHandled(Value promise);

// The embedder's job context (embed.h setJobContextHooks), per thread: the
// context a job runs in, for an embedder that runs several realms of its own
// on one global object, where a function has no realm the runtime knows.
// `capture` names the context current where a reaction is subscribed (a
// `then`, an `await`) or a thenable adopted; 0 is none. A handler subscribed
// under a context other than 0 is wrapped so its job runs between `enter`
// and `leave` of it (leave on a throw too). With no hooks installed nothing
// is captured and nothing wrapped: one branch per subscription.
struct JobContextHooks {
    uint64_t (*capture)() = nullptr;
    void (*enter)(uint64_t context) = nullptr;
    void (*leave)(uint64_t context) = nullptr;
};
void rtSetJobContextHooks(const JobContextHooks& hooks);
const JobContextHooks* rtJobContextHooks();  // null with none installed

// Test accessors: the queue's depth and the registry's population, so a unit
// test can pin "one await of a settled promise is ONE job" and the
// parked/unparked transitions without capturing stderr.
size_t rtMicrotaskQueueLength();
size_t rtParkedRejectionCount();

}  // namespace bronze::runtime
