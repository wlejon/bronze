// THE SINGLE-TICK RULE, pinned the only way a program can see it: race an
// async function's awaits against a chain of `then`s and print who gets there
// first.
//
// Every line of the expectation was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.7.5.3 Await step 2 is PromiseResolve(%Promise%, value), and 27.2.4.7
//   step 2 says PromiseResolve returns its argument UNCHANGED when that
//   argument is already a promise whose constructor is %Promise%. So awaiting
//   an intrinsic promise allocates no wrapper and adopts nothing: the
//   resumption is subscribed directly to the promise the program handed over.
// * 27.7.5.3 step 5 then subscribes the resumption with PerformPromiseThen,
//   which for an already-fulfilled promise enqueues exactly ONE job
//   (27.2.5.4.1 step 8).
//
// One job — the same one `p.then(f)` on that promise would cost. That is the
// whole of the ES2019 change (the earlier reading wrapped the promise and then
// adopted it, costing three), and it is what makes the two sequences below
// advance in LOCKSTEP: one await per one `then`.
//
// A plain value is awaited for the same one tick by the other arm of 27.2.4.7:
// it is wrapped in a promise which resolving with a non-object fulfills
// immediately (27.2.1.3.2 step 8), so PerformPromiseThen again queues one job.
// The second `await` below is a plain string precisely so that both arms are
// pinned by the same interleaving.
//
// Reading the expectation: `counter` and the `then` chain are one tick apart
// throughout. If an await ever cost two ticks, every `await N` line would slide
// one place later and the two sequences would separate — which is exactly the
// failure this case exists to catch.

async function counter() {
  console.log('await 1');
  await Promise.resolve('x');
  console.log('await 2');
  await 'y';
  console.log('await 3');
}

counter();

Promise.resolve()
  .then(function () { console.log('then 1'); })
  .then(function () { console.log('then 2'); })
  .then(function () { console.log('then 3'); });

console.log('sync end');
