// Profile-guided regions around getters and calls: a recorded run says a
// value read through a getter (or returned by a call) is always a Number, so
// the compiler builds a fast copy whose arithmetic and compares are F64 and
// guards the value where it lands. When a later value is not a Number the
// guard fails into the slow copy, which resumes after the read: every getter
// and every call runs exactly as many times as in an unguarded run. The
// counters below are the check, and the mixed phis carry Numbers and
// non-Numbers through one loop.

let reads = 0;
let calls = 0;
const log = [];

class Pt {
  constructor(x, y) { this._x = x; this._y = y; }
  get x() { reads++; return this._x; }
  get y() { reads++; return this._y; }
}

function len2(p) { calls++; return p.x * p.x + p.y * p.y; }

function scan(ps, r) {
  let hits = 0;
  let total = 0;
  for (let i = 0; i < ps.length; i++) {
    const p = ps[i];
    const d = len2(p);
    if (d < r * r) hits++;
    if (p.x > p.y) total += p.x - p.y;
    else total += d - p.x;
  }
  return [hits, total];
}

// A phi that is a Number on some edges and a string or object on others.
function mixed(n, k) {
  let v = 0;
  let s = 0;
  for (let i = 0; i < n; i++) {
    if (i === k) v = "s" + i;
    else if (i === k + 1) v = { valueOf() { log.push("vo" + i); return i; } };
    else if (i === k + 2) v = undefined;
    else v = typeof v === "number" ? v + i : i;
    if (v < 50) s += 1;
    if (v == 7) s += 100;
    s += v > 3 ? 2 : 0;
  }
  return [typeof v, v, s];
}

const pts = [];
for (let i = 0; i < 64; i++) pts.push(new Pt((i * 7) % 13 - 6, (i * 5) % 11 - 5 + 0.5));
const train = JSON.parse('{"surprise": false}');
let sum = 0;
for (let r = 0; r < 400; r++) { const o = scan(pts, 6); sum += o[0] + o[1]; }
for (let r = 0; r < 400; r++) { const o = mixed(40, train.surprise ? 3 : 100); sum += o[2]; }
console.log("warm", sum, reads, calls);

reads = 0; calls = 0;
const odd = pts.slice(0, 8);
odd[2] = new Pt("3", 4);
odd[4] = new Pt(-0, NaN);
odd[5] = new Pt(2147483647, 2147483648);
odd[6] = new Pt({ valueOf() { log.push("vx"); return 2; } }, 1);
odd[7] = new Pt(undefined, null);
console.log("odd", scan(odd, 6).join(" "), reads, calls, log.join(","));

log.length = 0;
for (let k = 0; k < 10; k += 3) console.log("mixed", k, mixed(12, k).join(" "));
console.log("mixed log", log.join(","));
