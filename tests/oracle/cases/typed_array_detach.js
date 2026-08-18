// A transferred buffer leaves its views behind: element reads answer
// undefined, writes are discarded, and the length family answers 0 — the
// out-of-bounds witness of 10.4.5.9 through 23.2.4.2-4. The case walks every
// access path: the dynamic element ops, the proven typed ops (coercing
// positions), the constant-key caches, and the helpers behind them all.
const buf = new ArrayBuffer(32);
const a = new Float64Array(buf);
a[0] = 1.5;
a[3] = 6.25;
console.log(a.length, a.byteLength, a.byteOffset, a[0], a[3]);

const moved = buf.transfer();
console.log(a.length, a.byteLength, a.byteOffset);
console.log(a[0], a[3], a[31]);
a[0] = 99;
console.log(a[0]);
a["1"] = 55;
console.log(a["1"], a[1]);

// The bytes went WITH the transfer: a fresh view over the new buffer sees
// what `a` wrote before the detach, and the old buffer reports empty.
const b = new Float64Array(moved);
console.log(b.length, b[0], b[3]);
console.log(buf.byteLength, buf.detached, moved.byteLength);

// The coercing read path: ToNumber(undefined) is NaN.
const sink = new Float64Array(2);
sink[0] = a[0] + 1;
sink[1] = a[3] * 2;
console.log(sink[0], sink[1]);

// A loop that was valid, then detached under the SAME compiled code: the
// validity check is asked again after the call, never cached across it.
function sum(v, n) {
    let t = 0;
    for (let i = 0; i < n; i++) t += v[i];
    return t;
}
const buf2 = new ArrayBuffer(16);
const c = new Float64Array(buf2);
c[0] = 3;
c[1] = 4;
console.log(sum(c, 2));
buf2.transfer();
console.log(sum(c, 2));

// 10.4.5.16's order: the stored value converts FIRST, so a valueOf that
// transfers the buffer away turns the store into a discard, not a write
// through a dead window.
const buf3 = new ArrayBuffer(8);
const d = new Float64Array(buf3);
d[0] = 2.5;
d[0] = { valueOf() { buf3.transfer(); return 7; } };
console.log(d.length, d[0]);

// A shrinking resize closes the window the same way — and growing it back
// REOPENS it: bytes below the shrink survive, the regrown tail is zeroed
// (25.1.5.5), and the write that arrived while the window was closed left
// no trace.
const rbuf = new ArrayBuffer(32, { maxByteLength: 64 });
const r = new Float64Array(rbuf, 0, 4);
r[0] = 1.25;
r[3] = 6.5;
console.log(r.length, r[3]);
rbuf.resize(16);
console.log(r.length, r.byteLength, r[0], r[3]);
r[0] = 9;
rbuf.resize(32);
console.log(r.length, r[0], r[3]);

// An OFFSET view: out of bounds reports byteOffset 0 too (23.2.4.4), and
// validity comes back when the buffer regrows past its window.
const o = new Float64Array(rbuf, 16, 2);
console.log(o.length, o.byteOffset);
rbuf.resize(24);
console.log(o.length, o.byteOffset, o[0]);
rbuf.resize(32);
console.log(o.length, o.byteOffset, o[0]);
