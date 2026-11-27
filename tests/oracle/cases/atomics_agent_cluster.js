// 25.4.3.14 Atomics.wait, 25.4.3.12 Atomics.notify, and 25.4.3.15
// Atomics.waitAsync non-blocking / synchronous edge cases. Async resolution via
// Atomics.notify is covered in cases/atomics_wait_async.js.
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
