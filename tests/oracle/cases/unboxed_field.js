// A field the write audit certified is read as a RAW DOUBLE: no tag test, no
// ToNumber helper, no phi — one bitcast.
//
// That is sound because bronze's Value is NaN-boxed with the numbers at the
// BOTTOM of the encoding, so a Number's sixty-four bits are its double's
// sixty-four bits and a slot holding one already IS raw f64 storage. Nothing
// about the representation forks, and that is what most of this case is for:
// the same instances go to `JSON.stringify`, to `for-in`, to `Object.keys`, to
// a computed read, to a property descriptor and to `Object.defineProperty`, and
// each has to answer exactly what it answers for any other object.
//
// `-0`, `NaN` and `±Infinity` are the three doubles whose identity a bitcast
// could lose, so each is carried into a field, back out through the raw path,
// and checked by something that can tell it from its neighbour. Their bit
// patterns matter to more than arithmetic: a NaN outside the canonical pattern
// collides with the value model's TAG range, so the last check reads one back
// through a float view's bytes.

class Vec {
  constructor() {
    this.x = 0;
    this.y = 0;
    this.z = 0;
  }
}

// The fields are filled HERE and not in the constructor, because a class
// constructor's parameters have no proven signature — the harvest sees
// `this.x = <a dynamic parameter>` and types the field `dynamic`, which is a
// separate gap and not the one this case pins. `make` is an ordinary
// direct-callable function, so its parameters are joined over every call this
// compilation can see, and every one of them passes a number.
function make(x, y, z) {
  const v = new Vec();
  v.x = x;
  v.y = y;
  v.z = z;
  return v;
}

function dot(a, b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

function scaleInto(v, k) {
  v.x = v.x * k;
  v.y = v.y * k;
  v.z = v.z * k;
  return v;
}

// A loop, so the accumulator is a block parameter and the field type has to be
// good enough to name one.
function lengthSq(v) {
  let acc = v.x * v.x;
  for (let i = 0; i < 2; i++) acc = acc + v.y * v.y;
  return acc + v.z * v.z - v.y * v.y;
}

const a = make(1, 2, 3);
const b = make(4, 5, 6);
console.log("dot=" + dot(a, b));
console.log("lengthSq=" + lengthSq(a));
console.log("scaled=" + dot(scaleInto(make(1, 2, 3), 2), b));

// ---- the three bit patterns ---------------------------------------------

const zeros = make(0, -0, 0);
console.log("negzero=" + zeros.y);
console.log("negzero is -0: " + Object.is(zeros.y, -0));
console.log("1/negzero=" + 1 / zeros.y);
console.log("negzero+0 is +0: " + Object.is(zeros.y + 0, 0));

const inf = make(1 / 0, -1 / 0, 0);
console.log("inf=" + inf.x + "," + inf.y);
console.log("inf plus neg inf=" + (inf.x + inf.y));
console.log("1/inf=" + 1 / inf.x + " 1/neginf=" + 1 / inf.y);

const nan = make(0 / 0, 0 / 0, 1 / 0 - 1 / 0);
console.log("nan=" + nan.x + "," + nan.y + "," + nan.z);
console.log("nan self-equal: " + (nan.x === nan.x) + " " + (nan.y === nan.y));
console.log("isNaN: " + Number.isNaN(nan.x) + " " + Number.isNaN(nan.z));
console.log("nan arithmetic=" + (nan.x * 2 + 1));
console.log("Object.is(nan.x, nan.y): " + Object.is(nan.x, nan.y));

// The canonical pattern, read back as bytes. Every road into a slot
// canonicalizes a NaN, and it has to: the value model reads a non-canonical
// one's high half as a tag.
const view = new Float64Array(1);
const bits = new Uint32Array(view.buffer);
view[0] = nan.x;
console.log("nan bits=" + bits[1].toString(16) + ":" + bits[0].toString(16));

// ---- the same instance, read by dynamic code ----------------------------

const p = make(1.5, -0, 3);
console.log("keys=" + Object.keys(p).join(","));
console.log("json=" + JSON.stringify(p));
let order = "";
for (const k in p) order += k;
console.log("forin=" + order);
console.log("entries=" + JSON.stringify(Object.entries(p)));
console.log("values=" + Object.values(p).join(","));

// A computed read is a different path into the same slot.
const names = ["x", "y", "z"];
let viaKey = "";
for (let i = 0; i < names.length; i++) viaKey += p[names[i]] + ";";
console.log("computed=" + viaKey);

// The descriptor says an ordinary writable data property.
const desc = Object.getOwnPropertyDescriptor(p, "x");
console.log(
  "desc=" + desc.value + "," + desc.writable + "," + desc.enumerable + "," + desc.configurable);

// Redefining an EXISTING property leaves every attribute the descriptor does
// not mention alone (10.1.6.3), so this is a value write and nothing else — and
// the raw read has to see it.
Object.defineProperty(p, "x", { value: 42 });
console.log("defined=" + p.x + " " + dot(p, make(1, 0, 0)));
p.x = 43;
console.log("still writable=" + p.x);

// Now take the attribute away. The slot keeps its value, the sloppy-mode write
// is a silent no-op, and the read is unchanged.
Object.defineProperty(p, "z", { value: 7, writable: false });
p.z = 99;
console.log("frozen slot=" + p.z + " " + dot(p, make(0, 0, 1)));

// ---- allocation between the reads of one chain --------------------------
//
// Under GC stress every allocation collects, so the receiver MOVES between the
// first read of a chain and the last. The address a guard was built from is
// dead by then, and every dereference has to come from the root slot.

function alloc(n) {
  return make(n, n, n).x;
}

function movingChain(v) {
  return v.x * alloc(2) + v.y * alloc(3) + v.z * alloc(4);
}

let total = 0;
for (let i = 0; i < 200; i++) {
  total = total + movingChain(make(1, 2, 3));
}
console.log("moving=" + total);

// A live instance held across many collections still reads its own fields.
//
// The read here goes through its OWN function: an element read is dynamic, so
// passing `held[i]` to `dot` would join that parameter to `dynamic` and stand
// every site in it down — which is correct, and would quietly turn the rest of
// this case into a test of the boxed path.
function dotAny(a, b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
const held = [];
for (let i = 0; i < 64; i++) held.push(make(i, i + 0.5, -i));
let sum = 0;
for (let i = 0; i < held.length; i++) sum = sum + dotAny(held[i], held[i]);
console.log("held=" + sum);
console.log("held[7]=" + held[7].x + "," + held[7].y + "," + held[7].z);
