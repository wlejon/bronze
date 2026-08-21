// A field's type is a claim about every write in the PROGRAM, not about the
// class body.
//
// The harvest that types a field reads one class declaration. Every line below
// writes one of those fields from somewhere else — three lines down, through an
// alias, under a computed key, from inside a callback, after the instance has
// been put in an array — and the value that comes back has to be the value that
// was written, with the type it was written with. Before the write audit, a
// harvested `number` reached a control-flow merge as an f64 block parameter and
// the string came back `NaN`.
//
// The reads are deliberately written as merges (a loop, an if/else): a field
// type only becomes a hard unbox where lowering has to name a block
// parameter's type, so a straight-line read would pass either way.

function say(label, v) {
  console.log(label + "=" + v + " (" + typeof v + ")");
}

// ---- 1. the reported miscompile, exactly --------------------------------

class V {
  constructor() {
    this.x = 0;
  }
}

function readV(v) {
  let s = v.x;
  for (let i = 0; i < 2; i++) s = v.x;
  return s;
}

const v = new V();
say("v.x before", readV(v));
v.x = "hi";
say("v.x after", readV(v));

// The same field read through `this`, which is the shape the report gave. A
// method's receiver is whatever the call passed, so this is a second reason the
// claim cannot stand, and it has to answer the same.
class V2 {
  constructor() {
    this.y = 0;
  }
  read() {
    let s = this.y;
    for (let i = 0; i < 2; i++) s = this.y;
    let t;
    if (s) {
      t = this.y;
    } else {
      t = this.y;
    }
    return s + "/" + t;
  }
}
const v2 = new V2();
say("v2 before", v2.read());
v2.y = "hi";
say("v2 after", v2.read());

// ---- 2. through an alias ------------------------------------------------

class A {
  constructor() {
    this.p = 1;
  }
}
function alias(o) {
  return o;
}
function readA(a) {
  let s = a.p;
  for (let i = 0; i < 2; i++) s = a.p;
  return s;
}
const a = new A();
alias(a).p = "aliased";
say("a.p", readA(a));

// ---- 3. under a computed key -------------------------------------------

class B {
  constructor() {
    this.q = 2;
  }
}
function readB(b) {
  let s = b.q;
  for (let i = 0; i < 2; i++) s = b.q;
  return s;
}
const b = new B();
const key = "q";
b[key] = "computed";
say("b.q", readB(b));
say("b[key]", b[key]);

// ---- 4. from inside a callback -----------------------------------------

class C {
  constructor() {
    this.r = 3;
  }
}
function readC(c) {
  let s = c.r;
  for (let i = 0; i < 2; i++) s = c.r;
  return s;
}
const c = new C();
[0].forEach(function () {
  c.r = "callback";
});
say("c.r", readC(c));

// ---- 5. after escaping into a data structure ----------------------------

class D {
  constructor() {
    this.t = 4;
  }
}
function readD(d) {
  let s = d.t;
  for (let i = 0; i < 2; i++) s = d.t;
  return s;
}
const d = new D();
const bag = [];
bag.push(d);
const m = new Map();
m.set("d", d);
bag[0].t = "escaped";
say("d.t via array", readD(d));
m.get("d").t = "escaped again";
say("d.t via map", readD(d));

// ---- 6. through Object.defineProperty -----------------------------------

class E {
  constructor() {
    this.u = 5;
  }
}
function readE(e) {
  let s = e.u;
  for (let i = 0; i < 2; i++) s = e.u;
  return s;
}
const e = new E();
Object.defineProperty(e, "u", { value: "defined", writable: true, configurable: true });
say("e.u", readE(e));

// ---- 7. deleted, so the read is a hole ----------------------------------

class F {
  constructor() {
    this.w = 6;
  }
}
function readF(f) {
  let s = f.w;
  for (let i = 0; i < 2; i++) s = f.w;
  return s;
}
const f = new F();
say("f.w", readF(f));
delete f.w;
say("f.w deleted", readF(f));
console.log("has w = " + ("w" in f));

// ---- 8. an accessor over the name ---------------------------------------

class G {
  constructor() {
    this._g = 7;
  }
  get g() {
    return "accessor:" + this._g;
  }
  set g(value) {
    this._g = value;
  }
}
function readG(o) {
  let s = o.g;
  for (let i = 0; i < 2; i++) s = o.g;
  return s;
}
const g = new G();
say("g.g", readG(g));
g.g = 8;
say("g.g after set", readG(g));

// ---- 9. what still IS a number ------------------------------------------
//
// Nothing above writes `n`, so it keeps the class's claim — and the operators
// that can only produce a number keep it too.

class H {
  constructor() {
    this.n = 10;
  }
}
function readH(h) {
  let s = h.n;
  for (let i = 0; i < 2; i++) s = h.n;
  return s;
}
const h = new H();
say("h.n", readH(h));
h.n += 5;
say("h.n plus", readH(h));
h.n++;
say("h.n inc", readH(h));
h.n *= 2;
say("h.n times", readH(h));
h.n = h.n - 1;
say("h.n minus", readH(h));

// A number written from a place the harvest never looked is still a number.
function bump(o) {
  o.n = o.n / 2;
}
bump(h);
say("h.n bumped", readH(h));
