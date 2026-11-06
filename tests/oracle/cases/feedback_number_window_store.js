// A constant-index read window off one array, cut by stores to `this` the
// fast copy's shape tests cover (`Vector3.applyMatrix4`'s rows, each written
// to this.x / this.y / this.z before the next row is read): one number claim
// in front of all three rows answers their element tests. Then the arrays
// stop being all Numbers (a string, a hole, an object, too short, a proxy), `this` is
// the array whose elements are read, a setter replaces a field, and elements
// change between calls. Whatever the build guessed, the bytes are these.

const out = [];
const log = (...xs) => out.push(xs.join(" "));

class Vec {
  constructor(x, y, z) { this.x = x; this.y = y; this.z = z; }
  apply(m) {
    const x = this.x, y = this.y, z = this.z;
    const e = m.elements;
    const w = 1 / (e[3] * x + e[7] * y + e[11] * z + e[15]);
    this.x = (e[0] * x + e[4] * y + e[8] * z + e[12]) * w;
    this.y = (e[1] * x + e[5] * y + e[9] * z + e[13]) * w;
    this.z = (e[2] * x + e[6] * y + e[10] * z + e[14]) * w;
    return this;
  }
}

const ident = () => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
const mat = { elements: ident() };
mat.elements[12] = 2; mat.elements[13] = -1; mat.elements[14] = 0.5; mat.elements[0] = 1.5;
const fmt = (v) => [v.x, v.y, v.z].map((n) => (typeof n === "number" ? n.toFixed(4) : String(n))).join(",");

let v = new Vec(1, 2, 3);
for (let i = 0; i < 2000; ++i) { v.x = i % 7; v.y = 1; v.z = 2; v.apply(mat); }
log("warm", fmt(v));

// Elements that are not all Numbers, each row in turn.
for (const [at, val] of [[3, "2"], [5, "x"], [10, null], [14, { valueOf() { return 4; } }], [0, undefined]]) {
  const m = { elements: ident() };
  m.elements[at] = val;
  log("element", at, fmt(new Vec(1, 2, 3).apply(m)));
}
const holey = { elements: ident() };
delete holey.elements[9];
log("hole", fmt(new Vec(1, 2, 3).apply(holey)));
log("short", fmt(new Vec(1, 2, 3).apply({ elements: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0] })));

// Elements behind a proxy whose read of [6] writes this.x mid-call.
const target = new Vec(1, 2, 3);
const live = { elements: new Proxy(ident(), { get(t, k, r) { if (k === "6") { target.x = 100; return 2; } return Reflect.get(t, k, r); } }) };
log("getter", fmt(target.apply(live)));

// `this` is the array whose elements the rows read.
const self = Object.assign(ident(), { x: 1, y: 2, z: 3 });
self[12] = 5;
log("self", fmt(Vec.prototype.apply.call(self, { elements: self })), self.slice(0, 4).join(","));

// A setter on one instance's field: its writes run user code mid-call.
const watched = new Vec(1, 2, 3);
let seen = [];
let hidden = 1;
Object.defineProperty(watched, "x", { get() { return hidden; }, set(n) { seen.push(n.toFixed(2)); mat.elements[1] = "s"; hidden = n; } });
log("setter", fmt(watched.apply(mat)), seen.join(";"));
mat.elements[1] = 0;

// Elements change between calls, back to all Numbers.
for (let r = 0; r < 3; ++r) {
  mat.elements[r * 5] = r + 2;
  v = new Vec(1, 1, 1);
  for (let i = 0; i < 300; ++i) v.apply(mat);
  log("rewrite", r, fmt(v));
}
mat.elements[4] = 0.5;
mat.elements[4] = "0.5";
log("string again", fmt(new Vec(1, 2, 3).apply(mat)));
mat.elements[4] = 0.25;
log("number again", fmt(new Vec(1, 2, 3).apply(mat)));

console.log(out.join("\n"));
