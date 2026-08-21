// A class constructor's PARAMETERS, typed by the join over every `new C(...)`
// site the program contains.
//
// For a class whose binding never leaves `new` position, those sites are every
// site there will ever be, so the join is a proof and not a guess — the same
// standard a top-level `function` whose name never escapes is held to. That is
// what makes `this.x = x` a Number write, which is what the field-type audit
// needs to certify the name `x`, which is what turns `v.x` into a raw f64 load
// with no tag test and no helper.
//
// The shape is three.js's math classes: a default on every parameter, so a
// `new Vec()` with no arguments contributes the DEFAULT's type and not
// `undefined`. A parameter that took `undefined` from a bare `new` would join
// to Dynamic and stand the whole name down.
//
// `-0`, `NaN` and `±Infinity` go in through the parameters and come back out
// through the raw path, because those are the three doubles a bitcast could
// lose — and a NaN outside the canonical pattern collides with the value
// model's tag range, so the last one is read back through a float view's bytes.

class Vec {
  constructor(x = 0, y = 0, z = 0) {
    this.x = x;
    this.y = y;
    this.z = z;
  }
}

// `super(...)` carries a subclass constructor's parameters into the base's,
// and the base's write is the one the audit reads.
class Vec4 extends Vec {
  constructor(x, y, z, w) {
    super(x, y, z);
    this.w = w;
  }
}

// No constructor at all: the implicit `constructor(...args) { super(...args) }`
// forwards every argument positionally, so `new Point(7, 8, 9)` is a call site
// of `Vec`'s constructor as surely as `new Vec(7, 8, 9)` is.
class Point extends Vec {}

function dot(a, b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

// A loop, so the accumulator is a block parameter and the field type has to be
// good enough to name one.
function lengthSq(v) {
  let acc = v.x * v.x;
  for (let i = 0; i < 2; i++) acc = acc + v.y * v.y;
  return acc + v.z * v.z - v.y * v.y;
}

const a = new Vec(1, 2, 3);
const b = new Vec(4, 5, 6);
console.log("dot=" + dot(a, b));
console.log("lengthSq=" + lengthSq(a));
console.log("defaults=" + new Vec().x + "," + new Vec(7).y + "," + new Vec(7, 8).z);

const p = new Vec4(1, 2, 3, 4);
console.log("vec4=" + p.x + "," + p.y + "," + p.z + "," + p.w);
console.log("vec4 dot=" + (p.x * p.x + p.w * p.w));

const q = new Point(7, 8, 9);
console.log("point=" + q.x + "," + q.y + "," + q.z + " " + dot(q, q));

// ---- writes from outside the constructor --------------------------------
//
// The invariant the audit certifies is name-global and about the whole heap,
// not about the constructor: a Number written from anywhere preserves it, and
// the reads on either side of the write stay raw.

const m = new Vec(1, 1, 1);
m.x = 10;
m.y = m.x * 2;
m.z = m.z + 5;
console.log("mutated=" + m.x + "," + m.y + "," + m.z + " " + dot(m, m));

// ---- the three bit patterns ---------------------------------------------

const zeros = new Vec(0, -0, 0);
console.log("negzero=" + zeros.y + " is -0: " + Object.is(zeros.y, -0));
console.log("1/negzero=" + 1 / zeros.y);
console.log("negzero+0 is +0: " + Object.is(zeros.y + 0, 0));

const inf = new Vec(1 / 0, -1 / 0, 0);
console.log("inf=" + inf.x + "," + inf.y + " sum=" + (inf.x + inf.y));
console.log("1/inf=" + 1 / inf.x + " 1/neginf=" + 1 / inf.y);

const nan = new Vec(0 / 0, 0 / 0, 1 / 0 - 1 / 0);
console.log("nan=" + nan.x + "," + nan.y + "," + nan.z);
console.log("nan self-equal: " + (nan.x === nan.x) + " " + (nan.y === nan.y));
console.log("isNaN: " + Number.isNaN(nan.x) + " " + Number.isNaN(nan.z));
console.log("nan arithmetic=" + (nan.x * 2 + 1));
console.log("Object.is(nan.x, nan.y): " + Object.is(nan.x, nan.y));

const view = new Float64Array(1);
const bits = new Uint32Array(view.buffer);
view[0] = nan.x;
console.log("nan bits=" + bits[1].toString(16) + ":" + bits[0].toString(16));

// ---- the same instances, read by dynamic code ---------------------------

console.log("keys=" + Object.keys(p).join(","));
console.log("json=" + JSON.stringify(a));
let order = "";
for (const k in q) order += k;
console.log("forin=" + order);
console.log("values=" + Object.values(b).join(","));

const names = ["x", "y", "z"];
let viaKey = "";
for (let i = 0; i < names.length; i++) viaKey += a[names[i]] + ";";
console.log("computed=" + viaKey);

const desc = Object.getOwnPropertyDescriptor(a, "x");
console.log(
  "desc=" + desc.value + "," + desc.writable + "," + desc.enumerable + "," + desc.configurable);

// ---- allocation between the reads of one chain --------------------------
//
// Under GC stress every allocation collects, so the receiver moves between the
// first read of a chain and the last, and every dereference has to come from
// the root slot rather than from an address a guard was built from.

function alloc(n) {
  return new Vec(n, n, n).x;
}

function movingChain(v) {
  return v.x * alloc(2) + v.y * alloc(3) + v.z * alloc(4);
}

let total = 0;
for (let i = 0; i < 200; i++) {
  total = total + movingChain(new Vec(1, 2, 3));
}
console.log("moving=" + total);

const held = [];
for (let i = 0; i < 64; i++) held.push(new Vec(i, i + 0.5, -i));
let sum = 0;
for (let i = 0; i < held.length; i++) sum = sum + held[i].x * held[i].y;
console.log("held=" + sum);
console.log("held[7]=" + held[7].x + "," + held[7].y + "," + held[7].z);
