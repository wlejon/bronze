import { Body } from './base.js';
import { Rocket } from './sub.js';

class Vec {
  constructor(x, y, z) { this.x = x; this.y = y; this.z = z; }
  set(x, y, z = this.z) { this.x = x; this.y = y; this.z = z; return this; }
  add3(a, b, c) { return a + b + c; }
  scale(s) { this.x = this.x * s; this.y = this.y * s; this.z = this.z * s; return this; }
  pick(on, yes, no) { if (on === true) return yes; if (on === undefined) return -1; return no; }
  mul(k) { return this === undefined ? "no this" : this.x * k; }
  sum() { return this.x + this.y + this.z; }
  grow(s) { this.x = this.x * s; this.y = this.y * s; return this; }
}

function show(label, v) {
  console.log(label, typeof v.x, String(v.x), typeof v.y, String(v.y), typeof v.z, String(v.z));
}

// 1. A method extracted and called with another `this`, after the typed edge
//    is warm, with arguments of every kind.
function extracted() {
  const v = new Vec(0, 0, 0);
  for (let i = 0; i < 50; i++) v.set(i * 0.5, i + 1, i - 1);
  show("warm", v);
  const f = v.set;
  const other = { z: "kept" };
  f.call(other, "7", null);
  show("extracted", other);
  const g = Vec.prototype.set;
  const third = { z: 1 };
  g.apply(third, [undefined, true]);
  show("apply", third);
  try {
    f(1, 2, 3);
  } catch (e) {
    console.log("unbound this:", e instanceof TypeError);
  }
  const data = v;
  const h = data.set;
  h.call(v, "a", "b");
  show("named data", v);
  const { set } = Vec.prototype;
  set.call(v, "s", "t", "u");
  show("destructured", v);
}

// 2. A prototype method replaced after the typed edge has run.
function replaced() {
  const v = new Vec(1, 1, 1);
  let total = 0;
  for (let i = 0; i < 40; i++) {
    if (i === 20) {
      Vec.prototype.scale = function (s) { this.x = this.x + s; return this; };
    }
    v.scale(1.5);
    total += v.x;
  }
  console.log("replaced", total, v.x, v.y);
  for (let i = 0; i < 3; i++) {
    if (i === 1) delete Vec.prototype.scale;
    try {
      v.scale(2);
      console.log("scale ok", v.x);
    } catch (e) {
      console.log("scale gone", e instanceof TypeError);
    }
  }
}

// 3. A subclass override declared in another module, at a site typed on the
//    base class, with a missing defaulted argument.
function overridden() {
  const bodies = [new Body(0), new Rocket(0), new Body(1), new Rocket(2)];
  for (let t = 0; t < 30; t++) {
    for (let i = 0; i < bodies.length; i++) {
      bodies[i].step(t * 0.1);
      bodies[i].step(t * 0.1, 0.25);
    }
  }
  for (const b of bodies) console.log("body", b.constructor.name, b.x.toFixed(6), b.v.toFixed(6), b.energy(2).toFixed(6));
}

// 4. Missing, extra and non-number arguments at the same method.
function arities() {
  const v = new Vec(1, 2, 3);
  const out = [];
  for (let i = 0; i < 30; i++) {
    out.push(v.add3(i, 1, 2));
    if (i % 10 === 0) {
      out.push(v.add3(i));
      out.push(v.add3(i, 2));
      out.push(v.add3(i, 1, 2, 99));
      out.push(v.add3("s", i, null));
      out.push(v.add3({ valueOf() { return 5; } }, i, 1));
      out.push(v.add3(true, i, 0.5));
      out.push(v.add3(NaN, -0, 0));
    }
  }
  console.log("arities", out.map(String).join(","));
  const z = new Vec(0, 0, 0);
  z.set(-0, NaN, 1);
  console.log("negzero", Object.is(z.x, -0), Number.isNaN(z.y), z.z);
  for (let i = 0; i < 5; i++) z.set(1, 2);
  console.log("default", z.z);
  z.set(1, 2, undefined);
  console.log("explicit undefined", z.z);
  const p = [];
  for (let i = 0; i < 20; i++) {
    p.push(v.pick(i > 10, i, -i));
    p.push(v.pick(i > 15, 1));
    p.push(v.pick());
    if (i === 3) p.push(v.pick("yes", 1, 2), v.pick(1, 1, 2), v.pick(true, "y", "n"));
  }
  console.log("pick", p.join(","));
}

// 5. call / apply / Reflect.apply of a method that typed edges also reach.
function reflective() {
  const v = new Vec(2, 3, 4);
  let s = 0;
  for (let i = 0; i < 20; i++) s += v.add3(i, 0.5, 0.25) + v.grow(1.0625).x;
  console.log("direct", s);
  console.log("call", Vec.prototype.add3.call(v, "x", 1, 2));
  console.log("apply", Vec.prototype.add3.apply(null, [1, "y", 2]));
  console.log("reflect", Reflect.apply(Vec.prototype.add3, v, [1, 2, "z"]));
  console.log("reflect num", Reflect.apply(Vec.prototype.grow, { x: 1, y: "b", z: 2 }, [3]).y);
  const bound = v.add3.bind(v, 10);
  console.log("bound", bound(1, 2), bound("q"));
}

// 6. A method used as a callback: `this` is whatever the caller passes.
function callbacks() {
  const v = new Vec(2, 0, 0);
  let s = 0;
  for (let i = 0; i < 20; i++) s += v.mul(i);
  console.log("mul loop", s);
  console.log("map this", [1, 2, 3].map(v.mul, v).join(","));
  console.log("map no this", [1, 2].map(v.mul).join(","));
  console.log("map other", [1, 2].map(v.mul, { x: "ab" }).join(","));
  const fns = [v.sum, Vec.prototype.sum];
  console.log("sum other", fns.map((f) => f.call({ x: "a", y: 1, z: 2 })).join(","));
}

// 7. A getter of the same name on another class, at one site.
class Getter {
  constructor() { this.x = 100; }
  get add3() { return (a, b, c) => "getter:" + (a * 2) + ":" + (this.x + (b === undefined ? 0 : b) + (c === undefined ? 0 : c)); }
  get scale() { return 7; }
}
function getters() {
  const items = [new Vec(1, 1, 1), new Getter(), new Vec(2, 2, 2), new Getter()];
  const out = [];
  for (let r = 0; r < 10; r++) {
    for (let i = 0; i < items.length; i++) out.push(items[i].add3(r * 1.5, i, 1));
  }
  console.log("getters", out.slice(0, 8).join(","), out.length);
  try {
    items[1].scale(2);
  } catch (e) {
    console.log("getter value not callable", e instanceof TypeError);
  }
  const lit = { add3: 4, set: undefined };
  try {
    lit.add3(1, 2, 3);
  } catch (e) {
    console.log("literal value not callable", e instanceof TypeError);
  }
}

// 8. A throw out of a typed entry names the method, not the entry.
class Thrower {
  constructor() { this.n = 0; }
  bump(k, limit = 3) {
    this.n = this.n + k;
    if (this.n > limit) throw new RangeError("over " + this.n);
    return this.n;
  }
}
function throws() {
  const t = new Thrower();
  try {
    for (let i = 0; i < 10; i++) t.bump(1.5);
  } catch (e) {
    console.log("thrown", e.name, e.message, String(e.stack).includes("$typed"));
  }
}

extracted();
replaced();
overridden();
arities();
reflective();
callbacks();
getters();
throws();
