// f64_demote versions a loop whose counter is stepped by a value the loop
// does not change (`i += len`) on 1 <= step <= 2^31, so the counter can be an
// integer while the step is one and the loop as written runs for every other
// step: zero, negative, fractional, NaN, -0, huge. Each function is called
// hot with ordinary steps first, so the versioned code is the one compiled,
// then with each awkward step. Every loop that could run forever counts its
// trips and stops at a cap.
function opaque(x) { return Number(String(x)); }

// The step an entry parameter: the entry check takes it for an integer. The
// first loop makes `n` a size the bounds may use, as the FFT's bit reversal
// does for its `n`.
function stridedParam(a, n, step) {
  let s = 0, c = 0, i = 0;
  for (let j = 0; j < n; j++) s -= a[j] === undefined ? 0 : 1;
  for (i = 0; i < n; i += step) {
    const v = a[i];
    s += v === undefined ? 1000 : v;
    if (++c >= 40) break;
  }
  return c + ":" + s + ":" + i;
}

// The FFT's shape: the step is the outer loop's `len <<= 1`, an int32 that
// wraps negative past 2^30, and the index adds the inner counter and half.
function butterflies(a, n) {
  for (let j = 0; j < n; j++) a[j] = a[j] * 2;
  for (let len = 2; len <= n; len <<= 1) {
    const half = len >> 1;
    for (let i = 0; i < n; i += len) {
      for (let k = 0; k < half; k++) {
        const u = a[i + k];
        const t = a[i + k + half];
        a[i + k] = u + t;
        a[i + k + half] = u - 2 * t;
      }
    }
  }
}

// The same step pattern started where the shift wraps: 2^30 << 1 is -2^31,
// then 0, so the inner loop's step goes negative and then zero.
function wrapping(b, n, start) {
  let trips = 0, outer = 0, last = 0;
  // Makes `n` a size, as in stridedParam, without walking all of it.
  for (let j = 0; j < n; j++) if (b[j] === undefined) break;
  for (let len = start; len <= n; len <<= 1) {
    for (let i = 0; i < n; i += len) {
      last = i + (b[i] === undefined ? 0 : 1);
      if (++trips >= 30) break;
    }
    if (++outer >= 6) break;
  }
  return trips + ":" + outer + ":" + last + ":" + Object.is(last, -0);
}

// An int32 step from the caller, so any value of it reaches the versioned loop.
function strideInt(b, raw) {
  const step = raw | 0;
  let c = 0, i = 0, s = 0;
  for (i = 0; i < b.length; i += step) {
    s += b[i];
    if (++c >= 25) break;
  }
  return c + ":" + i + ":" + s;
}

// A product of integers past 2^31: the copy runs it.
function strideBig(b, raw) {
  const step = (raw | 0) * 4;
  let c = 0, i = 0, s = 0;
  for (i = 0; i < b.length; i += step) {
    s += b[i];
    if (++c >= 25) break;
  }
  return c + ":" + i + ":" + s;
}

// Past 2^53 an f64 counter rounds where an i64 would not.
function nearLimit(b, raw) {
  const step = raw | 0;
  const n = 2 ** 53 + 10;
  const seen = [];
  for (let i = 2 ** 53 - 2; i < n; i += step) {
    seen.push(i - 2 ** 53 + (b[i] === undefined ? 0 : 1000));
    if (seen.length >= 10) break;
  }
  return seen.join(",");
}

// A throw from inside the loop, and the counter's value at the throw.
function throwing(a, raw) {
  const step = raw | 0;
  let i = 0;
  try {
    for (i = 0; i < a.length; i += step) {
      if (a[i] === 7) throw new Error("seven at " + i);
    }
  } catch (e) {
    return e.message + " i=" + i;
  }
  return "no throw i=" + i;
}

// Allocation in the loop body, so a collection runs inside it under stress.
function allocating(b, raw) {
  const step = raw | 0;
  const out = [];
  for (let i = 0; i < b.length; i += step) {
    out.push({ i, sq: i * i + b[i] });
    if (out.length >= 50) break;
  }
  let s = 0;
  for (const o of out) s += o.sq - o.i;
  return out.length + ":" + s;
}

// The step read through a getter once, before the loop.
let getterReads = 0;
const holder = { get step() { getterReads++; return opaque(3); } };
function viaGetter(b) {
  const step = holder.step | 0;
  let c = 0;
  for (let i = 0; i < b.length; i += step) c += i + b[i];
  return c;
}

// A counter starting at -0: no trip leaves it -0, one trip makes it the step.
function negZeroStart(b, raw) {
  const step = raw | 0;
  let i = -0, s = 0;
  for (; i < b.length; i += step) s += b[i];
  return (Object.is(i, -0) ? "-0" : String(i)) + ":" + s;
}

const a = new Float64Array(64);
for (let i = 0; i < 64; i++) a[i] = i;
// Zeros, so a sum over it is a count of nothing; its length bounds the loops.
const z40 = new Float64Array(40), z100 = new Float64Array(100), zBig = new Float64Array(2 ** 20);
const empty = new Float64Array(0), z5 = new Float64Array(5), z1 = new Float64Array(1), z10 = new Float64Array(10);

for (let round = 0; round < 3; round++) {
  const out = [];
  for (const st of [3, 1, 5, 0, -2, 2.5, NaN, -0, 2 ** 31, 2 ** 31 + 1, 1e300, Infinity, -Infinity, 64, 63]) {
    out.push(stridedParam(a, 64, opaque(st)));
  }
  for (const n of [8, 16, 4, 2, 1]) {
    const b = new Float64Array(n);
    for (let i = 0; i < n; i++) b[i] = i + 1;
    butterflies(b, opaque(n));
    out.push(Array.from(b).join(" "));
  }
  out.push(wrapping(z5, opaque(2 ** 31 - 1), opaque(2 ** 30)));
  out.push(wrapping(z5, opaque(100), opaque(16)));
  for (const r of [3, 1, 0, -1, -7, 2 ** 31 - 1, 0.5, -0.5, 4294967299]) out.push(strideInt(z40, opaque(r)));
  for (const r of [1, 2 ** 18, 2 ** 29, 2 ** 30, -1]) out.push(strideBig(zBig, opaque(r)));
  for (const r of [3, 1, 4]) out.push(nearLimit(z10, opaque(r)));
  for (const r of [7, 2, 3, 5, 0x7fffffff]) out.push(throwing(a, opaque(r)));
  for (const r of [1, 3, 0, -4]) out.push(allocating(z100, opaque(r)));
  out.push(viaGetter(z10));
  for (const [b, r] of [[empty, 1], [z5, 2], [empty, 3], [z1, 0x7fffffff]]) out.push(negZeroStart(b, opaque(r)));
  if (round === 2) {
    for (const line of out) console.log(line);
    console.log("getter reads " + getterReads);
  }
}
