// Profile-guided guarded regions (lower/guard_region.h, "PROFILE-GUIDED
// REGIONS"): values a profile may call Numbers that stop being Numbers in the
// middle of a loop. Built on its own profile the surprises poison the regions;
// built on the oracle-feedback suite's adversarial profile every value here is
// promoted, so each surprise fails a guard and finishes in the slow copy.
// Either way the bytes are these.

const data = JSON.parse('[1, 2.5, -3, 4, 0, 7, 2147483647, 6, -2147483648, 5]');
const odd = [
  "5", "abc", undefined, null, true, NaN, -0, 2147483648, -2147483649, 0.1 + 0.2,
  { valueOf() { calls.push("v" + this.n); return this.n; }, n: 3 },
  { valueOf() { calls.push("w"); return "7"; } },
  [4], [], 1e308 * 10, -Infinity,
];
const calls = [];

// Compares only: every operand an unboxed double in the fast copy.
class Box {
  constructor(a, b, c, d) { this.minX = a; this.maxX = b; this.minY = c; this.maxY = d; }
}
function inside(p, box) {
  return p.x >= box.minX && p.x <= box.maxX && p.y >= box.minY && p.y <= box.maxY;
}
function sameKey(a, b) { return a.k === b.k || a.k == b.j; }

// A painter-style comparator: `!==` then `-`, four times over.
function painter(a, b) {
  if (a.groupOrder !== b.groupOrder) return a.groupOrder - b.groupOrder;
  if (a.renderOrder !== b.renderOrder) return a.renderOrder - b.renderOrder;
  if (a.z !== b.z) return a.z - b.z;
  return a.id - b.id;
}

// A loop whose compare reads a call's result and a property: the guard on the
// call's result is after the call, so a failure never calls it again.
class Vec { constructor(x, y) { this.x = x; this.y = y; } dot(v) { calls.push("dot"); return this.x * v.x + this.y * v.y; } }
class Plane {
  constructor(n, c) { this.normal = n; this.constant = c; }
  distanceToPoint(p) { return this.normal.dot(p) + this.constant; }
}
function intersects(planes, center, radius) {
  const negRadius = -radius;
  for (let i = 0; i < planes.length; i++) {
    const d = planes[i].distanceToPoint(center);
    if (d < negRadius) return false;
  }
  return true;
}

// A loop-carried value (a phi) that is a Number for a while, then not.
function carried(xs, limit) {
  let acc = 0;
  let below = 0;
  for (let i = 0; i < xs.length; i++) {
    acc = (i % 4 === 3) ? xs[i] : acc + 1;
    if (acc < limit) below++;
    if (acc === acc) below += 2;
  }
  return [acc, below];
}

const box = new Box(0, 10, 0, 10);
let n = 0;
for (let r = 0; r < 300; r++) {
  for (let i = 0; i < data.length; i++) {
    if (inside({ x: data[i], y: data[(i + 3) % data.length] }, box)) n++;
    if (sameKey({ k: data[i], j: i }, { k: data[(i * 7) % data.length], j: 0 })) n += 2;
  }
}
console.log("numbers", n);
const lines = [];
for (let i = 0; i < odd.length; i++) {
  const p = { x: odd[i], y: odd[(i + 1) % odd.length] };
  lines.push(String(inside(p, box)) + "/" + sameKey({ k: odd[i], j: i }, { k: odd[i], j: odd[i] }));
}
console.log("odd", lines.join(" "));
console.log("calls", calls.join(","));
calls.length = 0;

const items = [];
for (let i = 0; i < 64; i++) items.push({ groupOrder: i % 2, renderOrder: (i * 5) % 3, z: (i * 7919) % 101, id: i });
let s = 0;
for (let k = 0; k < 20; k++) { const c = items.slice(); c.sort(painter); s += c[0].id * 100 + c[63].id; }
console.log("sorted", s);
items[5].z = "q"; items[9].renderOrder = undefined; items[10].renderOrder = NaN;
items[12].z = -0; items[13].z = 0; items[20].groupOrder = 2147483648; items[21].groupOrder = -0;
items[30].z = odd[10];
// Not sorted: a comparator that answers NaN leaves the order to the engine.
const mixed = [];
for (const i of [5, 9, 10, 12, 13, 20, 21, 30]) {
  for (const j of [4, 5, 9, 12, 13, 21, 30]) mixed.push(String(painter(items[i], items[j])));
}
console.log("mixed", mixed.join(" "));
console.log("calls", calls.join(","));
calls.length = 0;

const planes = [];
for (let i = 0; i < 6; i++) planes.push(new Plane(new Vec(i % 2, 1 - (i % 2)), i * 0.5));
let hits = 0;
for (let k = 0; k < 400; k++) if (intersects(planes, new Vec(k % 7, k % 5), 2.5)) hits++;
console.log("hits", hits);
calls.length = 0;
planes[3].constant = "1";
console.log("string constant", intersects(planes, new Vec(-9, -9), 2.5), intersects(planes, new Vec(9, 9), 2.5));
planes[3].constant = odd[10];
console.log("valueOf constant", intersects(planes, new Vec(9, 9), 2.5), calls.join(","));
calls.length = 0;
planes[2].constant = undefined;
console.log("undefined constant", intersects(planes, new Vec(9, 9), 2.5), calls.length);
console.log("nan radius", intersects(planes.slice(4), new Vec(1, 1), NaN), "string radius", intersects(planes.slice(4), new Vec(1, 1), "3"));

const xs = [];
for (let i = 0; i < 40; i++) xs.push(i);
for (let r = 0; r < 50; r++) carried(xs, 30);
console.log("carried", carried(xs, 30).join(" "));
xs[11] = "x"; xs[23] = undefined; xs[27] = -0; xs[31] = NaN; xs[35] = 2147483648;
console.log("carried odd", carried(xs, 30).join(" "));
xs[39] = { valueOf() { calls.push("v9"); return 9; }, n: 9 };
const last = carried(xs, 30);
console.log("carried valueOf", typeof last[0], last[1], calls.join(","));
