// Keyed element stores (`o[i] = v`) on the inline paths il2mir emits for an
// Array (an in-bounds element of an array with no named-properties object)
// and a TypedArray of a Number kind (an in-bounds element, a Number value),
// and on every edge that must leave them for the helper: holes, the length,
// frozen / sealed / non-writable / accessor elements, named properties,
// every typed kind's conversion, out-of-bounds and detached views. Each store
// runs in a loop hot enough to tier up, so every tier's lowering meets it.
//
// The barrier half: an array that has survived collections (old) is filled
// with freshly allocated objects (young) while garbage is churned, so a store
// that skipped the write barrier leaves the old array naming objects a minor
// collection freed or moved — read back below as wrong fields or a crash.

// Garbage by the byte rather than by the object: gc-stress collects at every
// allocation, so a few thousand 4 KB arrays (about 18 MB, past the nursery)
// make a plain run collect without making the stressed run slow.
function churn(n) {
  let keep = null;
  for (let i = 0; i < n; i++) keep = new Array(512);
  return keep.length;
}

function fill(arr, n, f) {
  for (let i = 0; i < n; i++) arr[i] = f(i);
  return arr;
}

// ---- plain arrays -------------------------------------------------------
{
  const a = new Array(16).fill(0);
  for (let r = 0; r < 300; r++) fill(a, 16, (i) => i * r);
  console.log('ints', a.join(','));
  for (let r = 0; r < 300; r++) fill(a, 16, (i) => i + 0.5);
  console.log('doubles', a.join(','));
  for (let r = 0; r < 300; r++) fill(a, 16, (i) => 's' + i);
  console.log('strings', a.join(','));
  const mixed = [1, 'two', null, undefined, true, 2.5, -0, NaN];
  const b = new Array(8).fill(0);
  for (let r = 0; r < 300; r++) fill(b, 8, (i) => mixed[i]);
  console.log('mixed', b.map((v) => (Object.is(v, -0) ? '-0' : String(v))).join(','));
}

// Holes inside the length are elements the store fills; past the length the
// array grows (the helper), leaving holes between.
{
  const h = [1, , 3, , 5];
  for (let r = 0; r < 300; r++) { h[1] = r; h[3] = -r; }
  console.log('holes', h.join(','), h.length, 1 in h, 3 in h);
  const g = [];
  for (let i = 0; i < 300; i++) g[i] = i;
  g[305] = 'far';
  console.log('grow', g.length, g[299], g[300], 303 in g, g[305]);
  const shifted = [0, 1, 2, 3, 4, 5];
  shifted.shift();
  shifted.shift();
  for (let r = 0; r < 300; r++) fill(shifted, 4, (i) => i * 10 + r % 3);
  console.log('shifted', shifted.join(','), shifted.length);
}

// Arrays that carry named properties or integrity levels.
{
  const named = [1, 2, 3];
  named.tag = 'x';
  for (let r = 0; r < 300; r++) fill(named, 3, (i) => i + r);
  console.log('named', named.join(','), named.tag);

  const frozen = Object.freeze([1, 2, 3]);
  for (let r = 0; r < 300; r++) frozen[1] = 99;
  console.log('frozen sloppy', frozen.join(','));
  let threw = 0;
  const strictStore = function (arr, i, v) { 'use strict'; arr[i] = v; };
  for (let r = 0; r < 300; r++) {
    try { strictStore(frozen, 0, r); } catch (e) { if (e instanceof TypeError) threw++; }
  }
  console.log('frozen strict threw', threw, frozen.join(','));

  const sealed = Object.seal([1, 2, 3]);
  for (let r = 0; r < 300; r++) strictStore(sealed, 2, r);
  console.log('sealed', sealed.join(','));

  // Non-extensible: an existing element stays writable, a new one is refused.
  const closed = Object.preventExtensions([1, 2, 3, 4]);
  threw = 0;
  for (let r = 0; r < 300; r++) {
    closed[0] = r;
    try { strictStore(closed, 4, r); } catch (e) { if (e instanceof TypeError) threw++; }
  }
  console.log('non-extensible', closed.join(','), closed.length, threw);

  // Frozen after it ran hot on the inline path.
  const late = [0, 0, 0];
  for (let r = 0; r < 300; r++) strictStore(late, r % 3, r);
  Object.freeze(late);
  threw = 0;
  for (let r = 0; r < 300; r++) {
    try { strictStore(late, 1, -1); } catch (e) { if (e instanceof TypeError) threw++; }
  }
  console.log('frozen late', late.join(','), threw);
}

// ---- the write barrier ---------------------------------------------------
{
  const old = new Array(64).fill(null);
  churn(4500);
  churn(4500);
  let bad = 0;
  for (let r = 0; r < 40; r++) {
    for (let i = 0; i < 64; i++) old[i] = { v: r * 1000 + i, s: 'k' + i, inner: [r, i] };
    churn(r % 5 === 4 ? 4500 : 1);
    for (let i = 0; i < 64; i++) {
      const o = old[i];
      if (o.v !== r * 1000 + i || o.s !== 'k' + i || o.inner[0] !== r || o.inner[1] !== i) bad++;
    }
  }
  console.log('barrier bad', bad, old[63].v, old[0].s);

  // Nested: an old array of old arrays, each refilled with young strings.
  const grid = [];
  for (let y = 0; y < 16; y++) grid.push(new Array(16).fill(''));
  churn(4500);
  churn(4500);
  for (let r = 0; r < 30; r++) {
    for (let y = 0; y < 16; y++) {
      const row = grid[y];
      for (let x = 0; x < 16; x++) row[x] = 'c' + (r + x + y);
    }
    churn(r % 10 === 9 ? 4500 : 1);
  }
  let ok = 0;
  for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) if (grid[y][x] === 'c' + (29 + x + y)) ok++;
  console.log('grid ok', ok);
}

// ---- typed arrays -------------------------------------------------------
const probes = [0, -0, 1, -1, 1.5, -1.5, 2.5, 254.5, 255, 256, 257, -129, 127, 128, 32767, 32768, 65535,
                65536, 65537, -32769, 2 ** 31, 2 ** 31 - 1, -(2 ** 31), 2 ** 32, 2 ** 32 + 5, -(2 ** 32) - 3,
                2 ** 53, -(2 ** 53), 2 ** 63, 1e20, -1e20, 0.1, 1 / 3, NaN, Infinity, -Infinity, 3.4e38, 1e-46];
const others = ['7', '', 'x', true, false, null, undefined, { valueOf() { return 42; } }, [5]];

function fmt(v) { return Object.is(v, -0) ? '-0' : String(v); }

function storeAll(ta, vals) {
  for (let i = 0; i < vals.length; i++) ta[i] = vals[i];
  return ta;
}

for (const C of [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array,
                 Float32Array, Float64Array]) {
  const ta = new C(probes.length);
  for (let r = 0; r < 200; r++) storeAll(ta, probes);
  console.log(C.name, Array.from(ta, fmt).join(','));
  const tb = new C(others.length);
  for (let r = 0; r < 200; r++) storeAll(tb, others);
  console.log(C.name, 'others', Array.from(tb, fmt).join(','));
}

// Computed values (an f64 the lowering holds unboxed), into each kind.
{
  const f = new Float64Array(8), g = new Float32Array(8), u = new Uint8ClampedArray(8), s = new Int16Array(8);
  for (let r = 0; r < 500; r++) {
    for (let i = 0; i < 8; i++) {
      const v = (i - 4) * 70.25 + r;
      f[i] = v; g[i] = v; u[i] = v; s[i] = v * 100;
    }
  }
  console.log('computed', Array.from(f).join(','), Array.from(g).join(','), Array.from(u).join(','),
              Array.from(s).join(','));
}

// Out of bounds, non-integral and negative indexes, a window over a larger
// buffer, a detached view, and BigInt kinds (the helper, and a TypeError for
// a Number).
{
  const ta = new Int32Array(4);
  for (let r = 0; r < 300; r++) { ta[4] = 9; ta[-1] = 9; ta[1.5] = 9; ta[3] = r; }
  console.log('oob', Array.from(ta).join(','), ta.length, Object.keys(ta).join(','));

  const buf = new ArrayBuffer(32);
  const win = new Uint16Array(buf, 6, 5);
  for (let r = 0; r < 300; r++) for (let i = 0; i < 6; i++) win[i] = 0x1234 + i + r;
  console.log('window', Array.from(new Uint8Array(buf)).join(','));

  const src = new Float64Array(4);
  const moved = src.buffer.transfer();
  for (let r = 0; r < 300; r++) src[0] = r;
  console.log('detached', src.length, src[0], new Float64Array(moved)[0]);

  const big = new BigInt64Array(2);
  let threw = 0;
  for (let r = 0; r < 300; r++) {
    big[0] = BigInt(r);
    try { big[1] = r; } catch (e) { if (e instanceof TypeError) threw++; }
  }
  console.log('bigint', big[0], big[1], threw);

  const half = new Float16Array(3);
  for (let r = 0; r < 300; r++) storeAll(half, [1.5, 65520, 1e-8]);
  console.log('float16', Array.from(half).join(','));
}

// A site that sees an Array, a TypedArray and a plain object in turn.
{
  const targets = [new Array(4).fill(0), new Float32Array(4), { length: 4 }, new Uint8Array(4)];
  for (let r = 0; r < 400; r++) {
    const t = targets[r % 4];
    for (let i = 0; i < 4; i++) t[i] = r + i + 0.25;
  }
  console.log('poly', targets.map((t) => Array.from(t).join('/')).join(' '));
}
