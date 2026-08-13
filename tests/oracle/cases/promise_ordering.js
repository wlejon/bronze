// The JOB QUEUE, observed as the only thing a program can observe about it:
// the order lines print in.
//
// Every line of the expectation was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.2.3.1 step 12: the executor runs SYNCHRONOUSLY, inside the `new
//   Promise` call, before the constructor returns.
// * 27.2.5.4.1 PerformPromiseThen step 8: `then` on an ALREADY SETTLED promise
//   still enqueues a job. A handler never runs synchronously, so nothing a
//   `then` does can be observed before the current script finishes.
// * 27.2.5.4.1 step 9: `then` on a PENDING promise appends a reaction record
//   and enqueues nothing; the job appears when the promise settles.
// * 9.5: the queue is FIFO, and a job enqueued by a job runs in the same
//   checkpoint, after everything already queued.
// * 27.2.5.4 step 4: `then` returns a NEW promise, and the handler's return
//   value resolves it (27.2.2.1 step 1.h) — which is what makes a chain of
//   `then`s one tick apart rather than all in the same one.
//
// So the whole synchronous script runs first, then everything queued while it
// ran — A, B, C, then the `catch` — and only after those, D: D was not queued
// until C had returned, which puts it behind a job that was queued before it.

console.log('sync start');

const p = new Promise(function (resolve) {
  console.log('executor');
  resolve(1);
});

p.then(function (v) { console.log('then A ' + v); });
p.then(function (v) { console.log('then B ' + v); });

Promise.resolve(2).then(function (v) {
  console.log('then C ' + v);
  return v + 1;
}).then(function (v) {
  console.log('then D ' + v);
});

// A rejection takes the same path through the queue as a fulfillment, and a
// `catch` is `then(undefined, onRejected)` (27.2.5.1), so this is queued after
// C and before whatever C queues.
Promise.reject('bad').catch(function (e) { console.log('caught ' + e); });

console.log('sync end');
