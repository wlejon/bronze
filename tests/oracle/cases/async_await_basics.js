// `async` / `await`, at the level a program first meets it: the call returns a
// promise, the body up to the first `await` runs synchronously, and everything
// after it runs from the job queue.
//
// Every line of the expectation was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.7.5.1 AsyncFunctionStart step 9: the body is evaluated IMMEDIATELY, in
//   the caller's turn, up to its first suspension. So `f()` prints its first
//   line before `f()` has returned anything.
// * 27.7.5.3 Await: the awaited value goes through PromiseResolve (27.2.4.7)
//   and the resumption is subscribed as a reaction — a JOB. Nothing after an
//   `await` can run in the same turn, even when the awaited value is already
//   available.
// * 27.7.5.1: a `return` from the body RESOLVES the function's promise, and an
//   uncaught throw REJECTS it. Neither ever throws at the call site: an async
//   function that throws on its first line still returns a promise.
// * 27.2.5.4.1 step 8 vs step 9: `then` on the already-rejected promise `boom`
//   returned enqueues a job at once, where `then` on the still-pending `p`
//   only subscribes — which is what puts the two answers in this order.
//
// The interleaving is the point. Three jobs are queued by the synchronous
// script (f's resumption, g's `then`, boom's `catch`) and run in that order;
// f's resumption resolves `p`, which queues a FOURTH job, and a job queued by
// a job goes to the back.

async function f() {
  console.log('f: before await');
  const v = await 10;
  console.log('f: after await ' + v);
  return v * 2;
}

// No `await` at all: the body runs to completion inside the call, so the
// promise it returns is already fulfilled when the call returns.
async function g() {
  return 7;
}

// Throws before any suspension. 27.7.5.1 still hands the caller a promise.
async function boom() {
  throw 'kaboom';
}

console.log('sync start');

const p = f();
console.log('called f');

const q = g();
console.log('called g');

const b = boom();

p.then(function (r) { console.log('f resolved ' + r); });
q.then(function (r) { console.log('g resolved ' + r); });
b.catch(function (e) { console.log('boom caught ' + e); });

console.log('sync end');
