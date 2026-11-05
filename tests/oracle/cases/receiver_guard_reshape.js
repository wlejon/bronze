// One receiver's shape tested by several property reads in a row, with
// something between them that reshapes it: a call that adds or deletes a
// property, a setter that adds one, Object.defineProperty in a callee, a
// getter that deletes. A later read may reuse an earlier guard only when
// nothing between could have moved the shape, so every read here must see
// the object as it is now. The reads sit on both sides of `&&` / `||` joins
// too, where the guard reaches the join from two edges.
class P {
  constructor(i) { this.a = i; this.b = i + 1; this.c = i + 2; }
}
function addProp(o) { o.z = 100; }
function delProp(o) { delete o.a; }
function redefine(o) { Object.defineProperty(o, "b", { get() { return -7; }, configurable: true }); }
function nothing(o) { return o.c; }

function readAround(o, f) {
  const x = o.a + o.b;
  f(o);
  const y = o.a + "/" + o.b + "/" + o.c + "/" + o.z;
  return x + ":" + y;
}

function joins(o, f) {
  let s = "";
  if (o.a && o.b) s += "ab";
  if (o.c > 0 || f(o)) s += "c";
  if (o.a !== undefined && o.b !== -7) s += "!";
  s += o.a + "," + o.b + "," + o.c + "," + o.z;
  if ((o.a && f(o)) || o.b) s += "|" + o.a + "," + o.b;
  return s;
}

const fns = [nothing, addProp, delProp, redefine];
const out = [];
for (let r = 0; r < 3; r++) {
  for (let k = 0; k < fns.length; k++) {
    // Warm on `nothing` first, then meet each reshaper.
    for (let i = 0; i < 200; i++) readAround(new P(i), nothing);
    out.push(readAround(new P(k + 1), fns[k]));
    for (let i = 0; i < 200; i++) joins(new P(i + 1), nothing);
    out.push(joins(new P(k + 1), fns[k]));
  }
}
console.log(out.join(" "));

// A setter on the prototype that adds own properties when assigned through.
class Q {
  constructor() { this.x = 1; this.y = 2; }
  set w(v) { this.extra = v; delete this.x; }
}
function viaSetter(q, v) {
  const before = q.x + q.y;
  q.w = v;
  return before + ";" + q.x + ";" + q.y + ";" + q.extra;
}
let s = "";
for (let i = 0; i < 300; i++) s = viaSetter(new Q(), i);
console.log(s, viaSetter(new Q(), "end"));

// A getter that deletes and adds properties of the receiver it is read from.
const proto = {
  get g() { delete this.m; this.n = 5; return 9; },
};
function viaGetter(o) {
  const first = o.m + o.k;
  const g = o.g;
  return first + "|" + g + "|" + o.m + "|" + o.k + "|" + o.n;
}
let t = "";
for (let i = 0; i < 300; i++) {
  const o = Object.create(proto);
  o.m = i; o.k = 1;
  t = viaGetter(o);
}
console.log(t);

// The same receiver read across a join whose one side reshapes it.
function branchReshape(o, flip) {
  const a = o.a;
  if (flip) delete o.a; else o.b = o.b + 1;
  return a + "," + o.a + "," + o.b + "," + o.c;
}
const br = [];
for (let i = 0; i < 400; i++) {
  const v = branchReshape(new P(i), i % 97 === 0);
  if (i % 97 === 0 || i === 399) br.push(v);
}
console.log(br.join(" "));

// Truthiness joins on one receiver's fields, then a field read after.
function presence(o) {
  if (o.map && o.map.isTexture) return "tex:" + o.map.id;
  if (o.map || o.alt) return "some:" + (o.map ? o.map.id : o.alt);
  return "none";
}
const objs = [
  { map: { isTexture: true, id: 1 }, alt: null },
  { map: null, alt: "x" },
  { map: undefined, alt: 0 },
  { map: { isTexture: false, id: 4 }, alt: null },
];
let ps = "";
for (let i = 0; i < 1000; i++) ps = objs.map(presence).join(",");
objs[0].map = { id: 9, isTexture: 1 };
delete objs[3].map.isTexture;
console.log(ps, objs.map(presence).join(","));
