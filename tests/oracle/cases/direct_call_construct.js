// `new C(...)` at a site in a loop, where compiled code may bump the
// instance and enter the constructor itself (bronze_abi.h, the inline `new`
// contract): what the constructor returns decides the result (an object
// replaces the instance, a primitive does not), new.target is the
// constructor, a class is constructible but not callable, the prototype is
// read at each construction (it may be replaced between them), and every
// other kind of target — a bound function, a Proxy, a derived class, a
// native, an arrow, a method — reaches the same site and is handled by the
// runtime. The constructors allocate, for the GC-stress re-run.

"use strict";
function Plain(a) { this.a = a; this.s = "p" + a; }
function RetObj(a) { this.a = a; return { replaced: a }; }
function RetPrim(a) { this.a = a; return 7; }
function RetMaybe(a) { this.a = a; if (a % 3 === 0) return [a]; }
function Target(a) { this.t = new.target === Target ? "Target" : String(new.target); this.a = a; }
class Base { constructor(a) { this.a = a; this.k = "base"; } }
class Derived extends Base { constructor(a) { super(a + 100); this.d = new.target.name; } }
class Fields { x = 1; y = this.x + 1; constructor(a) { this.a = a; } }
const BoundPlain = Plain.bind(null, "bound");
const ProxyPlain = new Proxy(Plain, { construct(t, args) { return { proxied: args[0] }; } });
const arrow = (a) => a;
const method = { m(a) { return a; } }.m;

function make(C, a) { return new C(a); }
function show(o) {
  if (o instanceof Map) return "Map";
  if (Array.isArray(o)) return "[" + o.join(",") + "]";
  const proto = Object.getPrototypeOf(o);
  const pn = proto && proto.constructor ? proto.constructor.name : "null";
  return pn + JSON.stringify(o);
}

const targets = [Plain, RetObj, RetPrim, RetMaybe, Target, Base, Derived, Fields, BoundPlain, ProxyPlain, Map, Array, Object, arrow, method, Math.max, 5];
for (let round = 0; round < 2; round++) {
  const line = [];
  for (let i = 0; i < targets.length; i++) {
    for (let w = 0; w < 25; w++) make(Plain, w);
    try { line.push(show(make(targets[i], i))); } catch (e) { line.push(e.constructor.name); }
  }
  console.log(line.join("\n"));
}

// RetMaybe alternates between its object and the instance at one warm site.
let alt = "";
for (let i = 0; i < 9; i++) alt += show(make(RetMaybe, i)) + " ";
console.log(alt);

// The prototype is read at each construction.
function Proto(a) { this.a = a; }
const protos = [];
for (let i = 0; i < 6; i++) {
  if (i === 3) Proto.prototype = { tag: "second" };
  const o = make(Proto, i);
  protos.push((o.tag || "first") + ":" + (o instanceof Proto));
}
console.log(protos.join(" "));

// A class constructor called, not constructed, at the sites that construct
// it: a TypeError every time, through a call and through a method.
let calls = "";
const holder = { C: Base };
for (let i = 0; i < 4; i++) {
  make(Base, i);
  try { Base(i); calls += "called "; } catch (e) { calls += e.constructor.name + " "; }
  try { holder.C(i); calls += "called "; } catch (e) { calls += e.constructor.name + " "; }
  try { Base.call({}, i); calls += "called "; } catch (e) { calls += e.constructor.name + " "; }
}
console.log(calls.trim());

// Many constructions, each kept, across collections.
const keep = [];
for (let i = 0; i < 3000; i++) {
  const o = make(i % 2 ? Plain : Fields, i);
  if (i % 300 === 0) keep.push(o);
}
console.log(keep.map(o => o.a + (o.s || o.y)).join(","));
