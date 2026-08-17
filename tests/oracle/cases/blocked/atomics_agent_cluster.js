// BLOCKED: `Hard runtime error: unsupported: Atomics.wait is not implemented
// (it operates on an agent cluster; bronze programs are a single agent, so
// there is nothing to wait for and nothing to wake)`.
//
// The rest of 25.4 is implemented and pinned in cases/shared_memory_atomics.js:
// every memory access — load, store, the six read-modify-writes,
// compareExchange, isLockFree — over both SharedArrayBuffer- and
// ArrayBuffer-backed integer views. The three refused here are the ones that are
// not memory accesses at all but operations on a SECOND AGENT: `wait` blocks the
// caller until another agent notifies it, `notify` wakes agents in a wait list,
// and `waitAsync` is `wait` with a promise. bronze has one agent, no way to
// spawn another, and no wait list.
//
// `notify` could legally answer 0 today — "no agent was woken" is true — and
// that is deliberately not what bronze does. The loop that calls `notify` has a
// counterpart that calls `wait`, and `wait` cannot be answered at all; a
// spin-wait whose `notify` silently succeeds becomes an infinite loop instead of
// a diagnostic. All three are refused so the error lands on the first line of
// the protocol rather than at its deadlock.
//
// What the expectation below owes, and why every line of it is decidable for a
// single agent — which is what makes this promotable rather than a wish:
//
//   - 25.4.3.14 DoWait returns "not-equal" at step 12 when the value in memory
//     differs from the expected one, BEFORE it ever enters the wait list. With
//     one agent that answer can never change, so it is the whole of `wait` for
//     the case where the guard has already moved.
//   - Step 15 returns "timed-out" when the timeout is 0. A zero timeout is the
//     other half a single agent can answer honestly: nothing can notify it
//     inside zero milliseconds.
//   - A wait with a MATCHING value and no timeout is deliberately absent from
//     this case: on a single agent it never returns, and a test that hangs is
//     worse than one that fails.
//   - `Atomics.notify` returns the number of agents woken, which is 0 with an
//     empty wait list — and the wait list is always empty here.
//   - `waitAsync` (25.4.3.15) answers a RECORD, not a promise, in both of the
//     synchronous cases: `{async: false, value: "not-equal"}` and, for a zero
//     timeout, `{async: false, value: "timed-out"}`. Only a real wait produces
//     `{async: true, value: <promise>}`, and that is the case a single agent
//     cannot reach.
//   - `wait` is stricter than the accessors: 25.4.3.14 admits only Int32Array
//     and BigInt64Array, and requires the buffer to be SHARED — the ES2024
//     relaxation that let `load` and `store` take a plain ArrayBuffer did not
//     extend to waiting, because there is no one on the other side of a
//     non-shared buffer. So an Int16Array and a plain-backed Int32Array are both
//     TypeErrors.
const sab = new SharedArrayBuffer(8);
const i32 = new Int32Array(sab);

// Memory holds 0, so an expectation of 1 has already been overtaken.
console.log(Atomics.wait(i32, 0, 1));
console.log(Atomics.wait(i32, 0, 0, 0));

console.log(Atomics.notify(i32, 0, 1));
console.log(Atomics.notify(i32, 0));
console.log(Atomics.notify(i32, 1, 0));

const overtaken = Atomics.waitAsync(i32, 0, 1);
console.log(overtaken.async, overtaken.value);
const expired = Atomics.waitAsync(i32, 0, 0, 0);
console.log(expired.async, expired.value);

try {
    Atomics.wait(new Int16Array(sab), 0, 1);
} catch (e) {
    console.log(e instanceof TypeError);
}
try {
    Atomics.wait(new Int32Array(new ArrayBuffer(8)), 0, 1);
} catch (e) {
    console.log(e instanceof TypeError);
}

const big = new BigInt64Array(sab);
console.log(Atomics.wait(big, 0, 1n));
console.log(Atomics.wait(big, 0, 0n, 0));
