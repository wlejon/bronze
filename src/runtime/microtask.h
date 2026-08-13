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

// Test accessors: the queue's depth and the registry's population, so a unit
// test can pin "one await of a settled promise is ONE job" and the
// parked/unparked transitions without capturing stderr.
size_t rtMicrotaskQueueLength();
size_t rtParkedRejectionCount();

}  // namespace bronze::runtime
