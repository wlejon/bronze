// The contract an embedding host's frame loop rests on, pinned WITHOUT a host:
// after the program's synchronous half returns, ONE microtask checkpoint runs
// every job the program left behind — and every job those jobs enqueue in
// turn — to quiescence, in a single FIFO order shared by every producer.
//
// src/rt/rt.cpp performs that checkpoint after `bronze_main()`; a host with a
// frame loop performs the same one per frame through
// embed::drainMicrotasks(). This case is the standalone half of that pair, so
// what it pins is exactly what a host inherits: a "frame" that reschedules
// itself through the queue keeps running after main returns, an `await` costs
// one tick and no more, and a rejection whose handler was attached
// synchronously is settled inside the same checkpoint.
//
// Every line below was derived BY HAND from ECMA-262 (2024) before bronze was
// run on it. The clauses that decide it:
//
// * 9.4.1 / 9.4.2: jobs run in the order they were enqueued, one queue, and a
//   job enqueued BY a job joins the same queue behind what is already in it.
//   That single ordering is what interleaves the three producers below.
// * 27.2.5.4.1 step 8: `then` on an ALREADY-SETTLED promise enqueues its
//   reaction job immediately; on a pending one it only records the reaction,
//   and the job appears when the promise settles.
// * 27.7.5.3 step 2 + 27.2.4.7 step 2: `await v` for a non-promise `v` wraps
//   it in a promise which resolving with a non-object fulfills at once
//   (27.2.1.3.2 step 8), so step 5's PerformPromiseThen enqueues exactly ONE
//   job — the same one cost a `then` on a settled promise pays. This is why
//   the async sequence and the then-chain below advance in lockstep.
// * 27.2.4.6: `Promise.reject(r)` answers an already-rejected promise, so the
//   `catch` attached to it is a reaction on a settled promise — one job, by
//   the same clause as the fulfilled case.

// --- 1. The checkpoint drains to quiescence, not one pass -------------------
// Each call reschedules itself through the queue, so the loop's frames 1..3
// exist ONLY inside the post-main checkpoint: a drain that ran the queue it
// found and stopped would print `frame 0` and `frame 1` and lose the rest.
let n = 0;
function frame() {
  console.log('frame ' + n);
  if (n < 3) {
    n = n + 1;
    Promise.resolve().then(frame);
  }
}
frame();

// --- 2. One tick per await, against a then-chain of the same length --------
// `loop` runs synchronously to its first await (27.7.5.1: an async function
// body executes until it suspends), so `async 0` prints in the synchronous
// half; each resumption after that lands one job apart, exactly as each link
// of the chain below does.
async function loop() {
  console.log('async 0');
  await null;
  console.log('async 1');
  await null;
  console.log('async 2');
}
loop();

Promise.resolve()
  .then(function () { console.log('then 0'); })
  .then(function () { console.log('then 1'); })
  .then(function () { console.log('then 2'); });

// --- 3. A rejection settled inside the checkpoint --------------------------
// The handler was attached while the program was still running, so nothing
// here is an UNHANDLED rejection: the drain runs the reaction and the program
// observes the value. (unhandled_rejection.js pins the other half.)
Promise.reject('boom').catch(function (e) { console.log('caught ' + e); });

console.log('main end');

// The interleaving, job by job. Synchronous half enqueues, in source order:
//   J1 = frame (from Promise.resolve().then(frame), n now 1)
//   J2 = loop's first resumption
//   J3 = the chain's first handler
//   J4 = the catch handler
// and prints `frame 0`, `async 0`, `main end`.
//
// The checkpoint then runs, appending as it goes:
//   J1 -> `frame 1`, n=2, enqueues J5
//   J2 -> `async 1`,      enqueues J6
//   J3 -> `then 0`, resolves the chain's first promise, enqueues J7
//   J4 -> `caught boom`
//   J5 -> `frame 2`, n=3, enqueues J8
//   J6 -> `async 2`, loop returns (its promise has no reactions)
//   J7 -> `then 1`,       enqueues J9
//   J8 -> `frame 3`, n<3 is false, nothing enqueued
//   J9 -> `then 2`
// and the queue is empty.
