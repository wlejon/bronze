// Atomics.wait / Atomics.notify / Atomics.waitAsync async resolution contract.
const sab = new SharedArrayBuffer(16);
const i32 = new Int32Array(sab);
const big = new BigInt64Array(sab);

console.log(Atomics.wait.length, Atomics.notify.length, Atomics.waitAsync.length);

// Pending waitAsync resolved by Atomics.notify.
const pending = Atomics.waitAsync(i32, 0, 0);
console.log(pending.async, pending.value instanceof Promise);
let observed = "pending";
pending.value.then((v) => {
    observed = v;
    console.log("resolved:", v);
});

// Notification on a different index does not wake waiter on index 0.
console.log("notify other:", Atomics.notify(i32, 1, 1));
console.log("before notify:", observed);
console.log("notify index 0:", Atomics.notify(i32, 0, 1));

// Synchronous cases on BigInt64Array.
const bigNotEq = Atomics.waitAsync(big, 1, 1n);
console.log("big not-equal:", bigNotEq.async, bigNotEq.value);
const bigTimeout = Atomics.waitAsync(big, 1, 0n, 0);
console.log("big timeout:", bigTimeout.async, bigTimeout.value);

// Non-shared buffer notify returns 0 without throwing.
console.log("non-shared notify:", Atomics.notify(new Int32Array(new ArrayBuffer(8)), 0, 1));
