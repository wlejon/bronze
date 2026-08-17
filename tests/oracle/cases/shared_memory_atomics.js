// SharedArrayBuffer (ECMA-262 25.2) and the Atomics namespace (25.4), on a host
// with exactly ONE agent.
//
// That last clause is the whole design and it is worth stating before the
// assertions: bronze has no worker, no second thread and no `postMessage`, so a
// SharedArrayBuffer's bytes are shared with nobody and every operation below is
// an ordinary aligned load or store. Nothing here is racy, so nothing here is
// non-deterministic — which is what makes 25.4 pinnable in an oracle case at
// all. `Atomics.wait`, `waitAsync` and `notify` are the three that need a second
// agent, and bronze refuses them by name (cases/blocked/atomics_agent_cluster.js).
//
// Derived from ECMA-262 rather than from bronze's output. What each group pins:
//
// 1. The two BRANDS are different types, not one type with two names. 25.2.5.6
//    tags a SharedArrayBuffer "SharedArrayBuffer", and its prototype is not
//    ArrayBuffer.prototype — so `sab instanceof ArrayBuffer` is false, and so is
//    `ab instanceof SharedArrayBuffer`, in both directions.
// 2. The two MEMBER SETS are disjoint where the specification makes them so.
//    25.2 gives `grow`/`growable`; 25.1.6 gives `resize`/`resizable`/`transfer`/
//    `detached`. Neither surface carries the other's, which `in` reports and a
//    read answers `undefined`.
// 3. `grow` is `resize` that cannot shrink (25.2.5.4 step 8's RangeError), and
//    the bytes it adds are zero.
// 4. A view over a SharedArrayBuffer is an ordinary view: same constructor, same
//    `buffer` identity, same aliasing between two views over one buffer.
// 5. `Atomics.store` returns the CONVERTED value and not the stored bytes
//    (25.4.3.11 returns `v`): storing 300 into a Uint8Array answers 300 and
//    leaves 44 in memory. Every read-modify-write returns the OLD value.
// 6. The arithmetic is MODULAR at the element's width, and signed where the
//    element is: 100 added to an Int8Array element holding -56 gives 44, and
//    `Atomics.and`/`or`/`xor` are bitwise on those same wrapped bytes.
// 7. `compareExchange` compares the TRUNCATED expected value against the stored
//    bytes, so an expectation of 300 matches a stored 44.
// 8. 25.4.3.1 admits eight element kinds and no others: the three float kinds
//    are a TypeError, and so is Uint8ClampedArray — whose store is a clamp
//    rather than a modulo, which would make a read-modify-write over it not an
//    arithmetic at all.
// 9. ES2024 dropped the requirement that the buffer be shared: every operation
//    works over a plain ArrayBuffer-backed integer view too, which is the case
//    almost all real code hits.
// 10. The two BigInt views go through ToBigInt, so mixing a Number in is the
//    same TypeError an ordinary element write gives.

// 1. The brands.
const sab = new SharedArrayBuffer(8);
const ab = new ArrayBuffer(8);
console.log(sab.byteLength, sab.growable, sab.maxByteLength);
console.log(Object.prototype.toString.call(sab), Object.prototype.toString.call(ab));
console.log(sab instanceof SharedArrayBuffer, sab instanceof ArrayBuffer);
console.log(ab instanceof ArrayBuffer, ab instanceof SharedArrayBuffer);
console.log(sab.constructor === SharedArrayBuffer, ab.constructor === ArrayBuffer);
console.log(SharedArrayBuffer[Symbol.species] === SharedArrayBuffer);

// 2. The member sets.
console.log("grow" in sab, "growable" in sab, "resize" in sab, "detached" in sab);
console.log("grow" in ab, "resize" in ab, "resizable" in ab, "detached" in ab);
console.log(sab.resize, sab.transfer, ab.grow, ab.growable);

// 3. Growing, and what it refuses.
const grown = new SharedArrayBuffer(4, { maxByteLength: 12 });
console.log(grown.byteLength, grown.maxByteLength, grown.growable);
new Uint8Array(grown).fill(255);
grown.grow(8);
const grownBytes = new Uint8Array(grown);
console.log(grown.byteLength, grownBytes[3], grownBytes[4], grownBytes[7]);
try {
    grown.grow(6);
} catch (e) {
    console.log(e instanceof RangeError, e.message);
}
try {
    sab.grow(16);
} catch (e) {
    console.log(e instanceof TypeError, e.message);
}
try {
    grown.grow(64);
} catch (e) {
    console.log(e instanceof RangeError);
}

// 4. Views over shared memory.
const i32 = new Int32Array(sab);
const u8 = new Uint8Array(sab);
i32[0] = 0x01020304;
console.log(i32.length, i32.buffer === sab, i32 instanceof Int32Array);
console.log(u8[0], u8[1], u8[2], u8[3]);
console.log(sab.slice(0, 4).byteLength, Object.prototype.toString.call(sab.slice(0, 4)));

// 5/6/7. Atomics over that view.
const a = new Int32Array(sab);
console.log(Atomics.store(a, 0, 5), Atomics.load(a, 0));
console.log(Atomics.add(a, 0, 3), Atomics.load(a, 0));
console.log(Atomics.sub(a, 0, 1), Atomics.load(a, 0));
console.log(Atomics.and(a, 0, 6), Atomics.or(a, 0, 1), Atomics.xor(a, 0, 2), Atomics.load(a, 0));
console.log(Atomics.exchange(a, 0, 42), Atomics.load(a, 0));
console.log(Atomics.compareExchange(a, 0, 42, 9), Atomics.load(a, 0));
console.log(Atomics.compareExchange(a, 0, 1, 100), Atomics.load(a, 0));

const bytes = new Uint8Array(4);
console.log(Atomics.store(bytes, 0, 300), Atomics.load(bytes, 0));
console.log(Atomics.compareExchange(bytes, 0, 300, 7), Atomics.load(bytes, 0));
const signed = new Int8Array(4);
console.log(Atomics.store(signed, 0, 200), Atomics.load(signed, 0));
console.log(Atomics.add(signed, 0, 100), Atomics.load(signed, 0));
console.log(Atomics.store(signed, 1, -1.9), Atomics.load(signed, 1));
console.log(Atomics.isLockFree(1), Atomics.isLockFree(2), Atomics.isLockFree(4));
console.log(Atomics.isLockFree(8), Atomics.isLockFree(3), Atomics.isLockFree(0));

// 8. The kinds 25.4.3.1 refuses.
try {
    Atomics.load(new Float64Array(2), 0);
} catch (e) {
    console.log(e instanceof TypeError, e.message);
}
try {
    Atomics.load(new Float16Array(2), 0);
} catch (e) {
    console.log(e instanceof TypeError, e.message);
}
try {
    Atomics.load(new Uint8ClampedArray(2), 0);
} catch (e) {
    console.log(e instanceof TypeError, e.message);
}
try {
    Atomics.load([1, 2], 0);
} catch (e) {
    console.log(e instanceof TypeError);
}
try {
    Atomics.load(new Int32Array(2), 5);
} catch (e) {
    console.log(e instanceof RangeError, e.message);
}

// 9. Plain, non-shared backing (ES2024).
const plain = new Int16Array(new ArrayBuffer(8));
console.log(Atomics.store(plain, 1, -3), Atomics.load(plain, 1));
console.log(Atomics.add(plain, 1, 4), Atomics.load(plain, 1));
console.log(Atomics.exchange(plain, 1, 40000), Atomics.load(plain, 1));

// 10. The BigInt views.
const big = new BigInt64Array(sab, 0, 1);
console.log(Atomics.store(big, 0, 2n ** 62n), Atomics.load(big, 0));
console.log(Atomics.add(big, 0, 1n), Atomics.load(big, 0));
console.log(Atomics.exchange(big, 0, -1n), Atomics.load(big, 0));
const ubig = new BigUint64Array(sab, 0, 1);
console.log(Atomics.load(ubig, 0));
try {
    Atomics.store(big, 0, 1);
} catch (e) {
    console.log(e instanceof TypeError);
}

console.log(Object.prototype.toString.call(Atomics), typeof Atomics.load);
console.log(sab, grown);

// 11. The wrap at the edges of each width, which is where the double-to-raw
// conversion earns its keep: `Atomics.store` answers the ToIntegerOrInfinity
// result and the ELEMENT holds that value modulo 2^width, signed if the kind is.
// -1 is the case that catches a conversion that wraps in the double domain:
// 2^64 - 1 is not a double, so `x + 2^64` rounds up and loses the answer.
const u32 = new Uint32Array(new SharedArrayBuffer(4));
console.log(Atomics.store(u32, 0, -1), Atomics.load(u32, 0));
console.log(Atomics.add(u32, 0, 1), Atomics.load(u32, 0));
console.log(Atomics.store(u32, 0, 2 ** 32 + 5), Atomics.load(u32, 0));
const s32 = new Int32Array(new ArrayBuffer(4));
console.log(Atomics.store(s32, 0, 2 ** 31), Atomics.load(s32, 0));
console.log(Atomics.store(s32, 0, Infinity), Atomics.load(s32, 0));
console.log(Atomics.store(s32, 0, NaN), Atomics.load(s32, 0));
console.log(Atomics.store(s32, 0, -1), Atomics.load(s32, 0));
const wide = new Uint8Array(new SharedArrayBuffer(2));
console.log(Atomics.store(wide, 0, -1), Atomics.load(wide, 0));
console.log(Atomics.sub(wide, 0, 1), Atomics.load(wide, 0));
