// A rejection NOTHING ever handles, and what a program can still observe
// afterwards.
//
// What this case pins is stdout, because stdout is what the oracle harness
// compares (tests/oracle/oracle_test.cpp reads the child's stdout and nothing
// else). The report itself is deliberately not on stdout: it goes to STDERR as
// `Unhandled promise rejection: <reason>` for the same reason an uncaught
// throw's report does — so a case can keep printing around one and still be
// pinnable byte-for-byte.
//
// So the expectation below is the claim that MATTERS and cannot be made any
// other way: a dropped rejection does not stop the program, does not stop the
// queue, and does not change the exit status. Every line after the two dropped
// rejections is there because the drain kept going.
//
// Every line was derived BY HAND from ECMA-262 before bronze was run on this
// file. The clauses that decide it:
//
// * 27.2.1.7 step 7 is HostPromiseRejectionTracker(promise, "reject"), which
//   the host is free to define. bronze PARKS the promise rather than reporting
//   it, because a handler may still arrive.
// * 27.2.5.4.1 step 10 marks a promise handled when a reaction is subscribed —
//   BEFORE the reaction runs. That is what lets a `catch` attached in the same
//   turn as the rejection cancel the report entirely, which is what `rescued`
//   below demonstrates: its handler runs, so nothing about it was ever
//   reported.
// * 27.7.5.1: an uncaught throw in an async body rejects that body's promise.
//   A rejection made that way is a rejection like any other, so `boom()` with
//   no handler is parked exactly as `Promise.reject` is.
// * 9.5: the checkpoint runs every queued job before it is over, so the
//   `rescued` handler runs even though two other promises are sitting parked.

console.log('start');

// Nobody will ever handle this. It is reported on stderr when the queue drains
// and the program's exit status is unaffected.
Promise.reject('dropped');

// Rejected in the same turn, handled in the same turn: parked at the rejection
// and un-parked by the `catch` before the queue is ever drained, so no report
// is made for it at all.
const rescued = Promise.reject('rescued');
rescued.catch(function (reason) {
  console.log('handled: ' + reason);
});

// The async-function spelling of the same drop.
async function boom() {
  throw 'async dropped';
}
boom();

// A rejection that is handled LATE — subscribed from inside a job rather than
// from the synchronous script. It is parked when the queue starts draining and
// un-parked before the drain reaches quiescence, which is why the report is
// made at the END of the drain and not the moment the queue first empties out
// a rejected promise.
const late = Promise.reject('late');
Promise.resolve().then(function () {
  late.catch(function (reason) {
    console.log('handled late: ' + reason);
  });
});

console.log('end');
