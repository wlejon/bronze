// The inline typed-array element paths (il_elem_lowering.cpp) against what
// 10.4.5 and the runtime answer, through sites that run often enough for every
// tier to lower them: each kind's conversion of every awkward value on a store
// (7.1.6-7.1.11, Uint8Clamped's round-half-even), each awkward index on a load
// and a store (only a canonical numeric string or a Number names an element;
// -0 is index 0; a fraction, a negative or an out-of-bounds index answers
// undefined and discards the store without asking the prototype), a view over
// a detached buffer, a length-tracking view whose buffer shrinks mid-loop,
// subarray views at offsets, a site that sees a plain array before a typed one,
// and a prototype index getter that an in-bounds integer index never consults.

function fmt(v) {
    if (typeof v === "number" && v === 0 && 1 / v < 0) return "-0";
    if (typeof v === "bigint") return v + "n";
    return String(v);
}
function row(a) {
    const out = [];
    for (let i = 0; i < a.length; i++) out.push(fmt(a[i]));
    return out.join(",");
}

// One store site and one load site per helper, run over every kind.
function store(a, i, v) { a[i] = v; }
function load(a, i) { return a[i]; }

const kinds = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array,
               Int32Array, Uint32Array, Float32Array, Float64Array];
const values = [NaN, Infinity, -Infinity, -0, 0.5, -0.5, 1.5, 2.5, -1.5, 127.5, 128, 255.5,
                256, -129, 65535.7, 65536, -32769, 2147483648, -2147483649, 4294967295.9,
                4294967296, 1e20, -1e20, 2 ** 53 + 2, 1.7976931348623157e308, 5e-324, 3.4028235677973366e38];

// --- conversions: every value into every kind, through the same store site ---
for (let round = 0; round < 3; round++) {
    for (const K of kinds) {
        const a = new K(values.length);
        for (let i = 0; i < values.length; i++) store(a, i, values[i]);
        if (round === 2) console.log(K.name, row(a));
    }
}

// --- the same table through sites that only ever see one kind, where the
//     element kind is known statically (each view is the function's own); each
//     then reads past either end, and over a detached buffer writes and reads
//     nothing ---
function i8(v, detach) {
    const a = new Int8Array(v.length);
    if (detach) a.buffer.transfer();
    for (let i = 0; i < v.length; i++) a[i] = v[i];
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i];
    return [a, fmt(s) + ":" + fmt(a[v.length]) + ":" + fmt(a[-1]) + ":" + fmt(a[0]) + ":" + a.length];
}
function u8(v, detach) {
    const a = new Uint8Array(v.length);
    if (detach) a.buffer.transfer();
    for (let i = 0; i < v.length; i++) a[i] = v[i];
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i];
    return [a, fmt(s) + ":" + fmt(a[v.length]) + ":" + fmt(a[-1]) + ":" + fmt(a[0]) + ":" + a.length];
}
function u8c(v, detach) {
    const a = new Uint8ClampedArray(v.length);
    if (detach) a.buffer.transfer();
    for (let i = 0; i < v.length; i++) a[i] = v[i];
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i];
    return [a, fmt(s) + ":" + fmt(a[v.length]) + ":" + fmt(a[-1]) + ":" + fmt(a[0]) + ":" + a.length];
}
function i16(v, detach) {
    const a = new Int16Array(v.length);
    if (detach) a.buffer.transfer();
    for (let i = 0; i < v.length; i++) a[i] = v[i];
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i];
    return [a, fmt(s) + ":" + fmt(a[v.length]) + ":" + fmt(a[-1]) + ":" + fmt(a[0]) + ":" + a.length];
}
function u16(v, detach) {
    const a = new Uint16Array(v.length);
    if (detach) a.buffer.transfer();
    for (let i = 0; i < v.length; i++) a[i] = v[i];
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i];
    return [a, fmt(s) + ":" + fmt(a[v.length]) + ":" + fmt(a[-1]) + ":" + fmt(a[0]) + ":" + a.length];
}
function i32(v, detach) {
    const a = new Int32Array(v.length);
    if (detach) a.buffer.transfer();
    for (let i = 0; i < v.length; i++) a[i] = v[i];
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i];
    return [a, fmt(s) + ":" + fmt(a[v.length]) + ":" + fmt(a[-1]) + ":" + fmt(a[0]) + ":" + a.length];
}
function u32(v, detach) {
    const a = new Uint32Array(v.length);
    if (detach) a.buffer.transfer();
    for (let i = 0; i < v.length; i++) a[i] = v[i];
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i];
    return [a, fmt(s) + ":" + fmt(a[v.length]) + ":" + fmt(a[-1]) + ":" + fmt(a[0]) + ":" + a.length];
}
function f32(v, detach) {
    const a = new Float32Array(v.length);
    if (detach) a.buffer.transfer();
    for (let i = 0; i < v.length; i++) a[i] = v[i];
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i];
    return [a, fmt(s) + ":" + fmt(a[v.length]) + ":" + fmt(a[-1]) + ":" + fmt(a[0]) + ":" + a.length];
}
function f64(v, detach) {
    const a = new Float64Array(v.length);
    if (detach) a.buffer.transfer();
    for (let i = 0; i < v.length; i++) a[i] = v[i];
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i];
    return [a, fmt(s) + ":" + fmt(a[v.length]) + ":" + fmt(a[-1]) + ":" + fmt(a[0]) + ":" + a.length];
}
// The finite values only, so each sum says something.
const finite = values.filter((v) => Number.isFinite(v) && Math.abs(v) < 1e300 && v !== 3.4028235677973366e38);
const mono = [["Int8Array", i8], ["Uint8Array", u8], ["Uint8ClampedArray", u8c], ["Int16Array", i16],
              ["Uint16Array", u16], ["Int32Array", i32], ["Uint32Array", u32], ["Float32Array", f32],
              ["Float64Array", f64]];
for (let round = 0; round < 3; round++) {
    for (const [name, fn] of mono) {
        const table = fn(values, false)[0];
        const sum = fn(finite, false)[1];
        const gone = fn(finite, true)[1];
        if (round === 2) {
            console.log("mono", name, row(table));
            console.log("mono", name, sum, gone);
        }
    }
}

// --- indices: fractional, negative, -0, out of bounds, strings ---
const keys = [0, -0, 1.5, -1, 3, 4, 2 ** 32, "1", "-0", "01", "1.0", "+1", "x", "Infinity", "NaN", "-1"];
function keyName(k) { return typeof k === "string" ? JSON.stringify(k) : fmt(k); }
for (let round = 0; round < 3; round++) {
    for (const K of [Int8Array, Uint8ClampedArray, Uint32Array, Float32Array, Float64Array]) {
        const a = new K([1, 2, 3, 4]);
        const got = [];
        for (const k of keys) got.push(keyName(k) + "=" + fmt(load(a, k)));
        for (const k of keys) store(a, k, 9);
        const own = Object.keys(a).join(" ");
        if (round === 2) {
            console.log(K.name, "get", got.join(" "));
            console.log(K.name, "set", row(a), "keys", own);
        }
    }
}

// --- the BigInt kinds take the helper: a Number store throws, a BigInt wraps ---
for (const K of [BigInt64Array, BigUint64Array]) {
    const a = new K(2);
    store(a, 0, 2n ** 64n + 5n);
    store(a, 1, -1n);
    let threw = "no";
    try { store(a, 0, 1); } catch (e) { threw = e instanceof TypeError ? "TypeError" : "other"; }
    console.log(K.name, row(a), fmt(load(a, 1)), threw);
}

// --- a value that is not a Number: ToNumber runs once, a valueOf included ---
{
    let calls = 0;
    const obj = { valueOf() { calls++; return 7.9; } };
    const a = new Int16Array(2);
    for (let i = 0; i < 50; i++) store(a, 0, obj);
    store(a, 1, "300.7");
    store(a, 5, obj); // out of bounds: still converted (10.4.5.16 step 1), then discarded
    console.log("valueOf", row(a), calls);
}

// --- a detached buffer: loads answer undefined, stores are discarded ---
function sumAll(a, n) {
    let t = 0;
    for (let i = 0; i < n; i++) t += load(a, i);
    return t;
}
{
    const buf = new ArrayBuffer(32);
    const f = new Float64Array(buf);
    const u = new Uint8Array(buf, 8, 8);
    for (let i = 0; i < 4; i++) store(f, i, i + 0.25);
    console.log("before", sumAll(f, 4), u.length, load(u, 0));
    const moved = buf.transfer();
    console.log("after", fmt(sumAll(f, 4)), f.length, u.length, fmt(load(u, 0)));
    store(f, 0, 42);
    store(u, 0, 42);
    console.log("discarded", fmt(load(f, 0)), new Float64Array(moved)[0]);
}

// --- a length-tracking view over a resizable buffer shrunk mid-loop ---
{
    const rab = new ArrayBuffer(16, { maxByteLength: 32 });
    const ta = new Uint16Array(rab);
    const fixed = new Uint16Array(rab, 4, 2);
    for (let i = 0; i < 8; i++) store(ta, i, 1000 + i);
    const seen = [];
    for (let i = 0; i < 8; i++) {
        if (i === 3) rab.resize(6);
        seen.push(fmt(load(ta, i)));
        store(ta, i, 7);
    }
    console.log("shrunk", seen.join(","), ta.length, fixed.length, fmt(load(fixed, 0)));
    rab.resize(12);
    console.log("regrown", row(ta), fixed.length, row(fixed));
}

// --- subarray views at offsets share the buffer, and bound at their own end ---
{
    const base = new Int32Array([10, 20, 30, 40, 50, 60]);
    const mid = base.subarray(2, 5);
    const tail = mid.subarray(1);
    for (let i = 0; i < 5; i++) store(mid, i, -i);
    console.log("subarray", row(base), row(mid), row(tail), mid.byteOffset, tail.byteOffset);
    const f = new Float32Array([1.1, 2.2, 3.3, 4.4]).subarray(1, 3);
    console.log("f32sub", fmt(load(f, 0)), fmt(load(f, 1)), fmt(load(f, 2)), fmt(load(f, -1)));
}

// --- one site, a plain array first and then every typed kind ---
function touch(a) {
    a[1] = a[0] + 0.75;
    return a[1];
}
{
    const plain = [3, 0];
    let t = 0;
    for (let i = 0; i < 200; i++) t += touch(plain);
    const out = ["plain=" + fmt(t)];
    for (const K of kinds) out.push(K.name + "=" + fmt(touch(new K([3, 0]))));
    out.push("sparse=" + fmt(touch([, 0])));
    console.log("poly", out.join(" "));
}

// --- a prototype index getter: never asked for an in-bounds integer index,
//     nor for a canonical numeric string; a non-canonical one is a name ---
{
    let asked = 0;
    Object.defineProperty(Float64Array.prototype, "0", { get() { asked++; return "proto0"; }, configurable: true });
    Object.defineProperty(Float64Array.prototype, "7", { get() { asked++; return "proto7"; }, configurable: true });
    Object.defineProperty(Float64Array.prototype, "01", { get() { asked++; return "proto01"; }, configurable: true });
    const a = new Float64Array([5.5, 6.5]);
    let t = 0;
    for (let i = 0; i < 100; i++) t += load(a, 0);
    console.log("getter", t, fmt(load(a, 7)), fmt(load(a, "0")), fmt(load(a, "01")), asked);
    delete Float64Array.prototype["0"];
    delete Float64Array.prototype["7"];
    delete Float64Array.prototype["01"];
}

// --- NaN bit patterns a Value must never carry: a negative or signalling NaN
//     read from a view is boxed as THE NaN, whichever path reads it (the
//     generic site, a kind-known one, a Float32 widened), so it stays a Number
//     through a plain array, a property and arithmetic ---
function nanBits() {
    const bytes = new Uint8Array(32);
    const f = new Float64Array(bytes.buffer);
    const g = new Float32Array(bytes.buffer, 16, 4);
    // Little-endian: 0xFFF8000000000000, 0x7FF0000000000001, then float32
    // 0xFF800001 and 0x7FC00000 | sign.
    bytes.set([0, 0, 0, 0, 0, 0, 0xF8, 0xFF, 1, 0, 0, 0, 0, 0, 0xF0, 0x7F,
               1, 0, 0x80, 0xFF, 0, 0, 0xC0, 0xFF]);
    const got = [];
    for (let i = 0; i < 2; i++) got.push(f[i]);
    for (let i = 0; i < 2; i++) got.push(g[i]);
    for (let i = 0; i < 2; i++) got.push(load(f, i));
    for (let i = 0; i < 2; i++) got.push(load(g, i));
    const holder = { v: 0 };
    const out = [];
    for (const v of got) {
        holder.v = v;
        const plain = [v];
        out.push(typeof plain[0] + "/" + fmt(holder.v) + "/" + fmt(v + 1) + "/" + Number.isNaN(plain[0]));
    }
    return out.join(" ");
}
for (let round = 0; round < 2; round++) nanBits();
console.log("nanbits", nanBits());
