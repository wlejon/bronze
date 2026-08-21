// Interprocedural identity: a class method's parameters, typed from the call
// sites the compilation can see through the RECEIVER's class.
//
// Every line below is a way for that join to be wrong, or to have been taken
// over callers it had no right to speak for — a `.call` that routes past the
// receiver, a call on an object whose class is not proven, a spread that breaks
// the position-by-position correspondence, a subclass override reached through a
// base-typed receiver, a class the analysis never modelled. Each has to answer
// exactly what it answered before the mechanism existed.

class Pt {
  constructor(x, y) {
    this.x = x;
    this.y = y;
  }
  copy(p) {
    this.x = p.x;
    this.y = p.y;
    return this;
  }
  dot(p) {
    return this.x * p.x + this.y * p.y;
  }
  label() {
    return "Pt";
  }
}

class Pt3 extends Pt {
  constructor(x, y, z) {
    super(x, y);
    this.z = z;
  }
  // `super.copy(p)` is a CALL, and the most common shape of one in a class
  // library. Read as a method escaping into a value it would poison `copy`
  // everywhere, on evidence that was a call.
  copy(p) {
    super.copy(p);
    this.z = p.z === undefined ? 0 : p.z;
    return this;
  }
  label() {
    return "Pt3";
  }
}

// The plain case: `p` is a Pt at every site that reaches this method.
console.log(new Pt(1, 2).dot(new Pt(3, 4)));

const a = new Pt(1, 2);
const b = new Pt(3, 4);
console.log(a.copy(b).dot(b));

// A Pt3 copying a Pt3.
const c = new Pt3(5, 6, 7);
console.log(c.copy(new Pt3(8, 9, 10)).z + "," + c.x + "," + c.y);

// A Pt3 copying a PT. The call `a.copy(b)` above has a receiver typed `Pt`, and
// a Pt receiver can be a Pt3 — so `Pt3::copy` is one of the methods that call
// contributes to, and its `p` is a Pt there. `p.z` is absent, and the answer has
// to be the one absence produces.
const e = new Pt3(1, 1, 1);
console.log(e.copy(new Pt(4, 5)).z + "," + e.x);

console.log(a.label() + "," + c.label());

// `.call` hands the method a receiver from the ARGUMENT list, which is not the
// class the call site's receiver named. `dot` gives up its parameter for it, and
// both the foreign receiver and the foreign argument still answer.
console.log(Pt.prototype.dot.call({ x: 100, y: 200 }, { x: 1, y: 1 }));

// A call on a receiver whose class is not proven: `o` joins two classes, so any
// `label` in the program could be the one that runs.
function nameOf(o) {
  return o.label();
}
console.log(nameOf(c) + "," + nameOf(a));

// A return value flowing back to its caller — through a conditional, where the
// two arms are two classes, and through a method that returns one.
class Maker {
  make(flag) {
    if (flag) return new Pt(1, 1);
    return new Pt3(2, 2, 2);
  }
  useIt(flag) {
    const p = this.make(flag);
    return p.x;
  }
  one() {
    return new Pt(9, 9);
  }
  useOne() {
    return this.one().x + this.one().y;
  }
}
const mk = new Maker();
console.log(mk.useIt(true) + "," + mk.useIt(false) + "," + mk.useOne());

// Recursion: the method is its own caller, so the join has to reach a fixpoint
// rather than chase itself.
class Chain {
  constructor(next) {
    this.next = next;
  }
  depth(node) {
    if (node === null) return 0;
    return 1 + this.depth(node.next);
  }
}
const n2 = new Chain(null);
const n1 = new Chain(n2);
console.log(new Chain(null).depth(n1));

// The three parameter lists that break the one-argument-per-parameter
// correspondence a joined signature IS: a spread at the CALL, and a rest or a
// default at the DECLARATION.
class Odd {
  sum(p, q) {
    return p.x + q.x;
  }
  rest(...items) {
    return items.length;
  }
  dflt(p, k = 5) {
    return p.x + k;
  }
}
const od = new Odd();
const pair = [new Pt(1, 0), new Pt(2, 0)];
console.log(od.sum(...pair) + "," + od.rest(1, 2, 3) + "," + od.dflt(new Pt(4, 0)));

// A method read as a value and invoked on something else entirely.
class Free {
  constructor() {
    this.v = 7;
  }
  get2() {
    return this.v * 2;
  }
}
const detached = new Free().get2;
console.log(detached.call({ v: 50 }));

// Two call sites that disagree: the join keeps the kind and loses the identity,
// and the site stays polymorphic.
class Poly {
  take(o) {
    return o.x;
  }
}
const py = new Poly();
console.log(py.take(new Pt(3, 0)) + "," + py.take({ x: "s" }));

// A computed call whose property name IS a fixed set the source spells out.
const m1 = "dot";
console.log(new Pt(3, 4)[m1](new Pt(1, 1)));

// A subclass this compilation never modelled — an anonymous class expression,
// which has no name for `extends` to resolve against and is collected by
// nothing. Its instances still run the base's methods.
function makeSub() {
  return class extends Pt {
    constructor(x, y) {
      super(x, y);
    }
    label() {
      return "Sub";
    }
  };
}
const Sub = makeSub();
const sub = new Sub(1, 2);
console.log(sub.dot(new Pt(2, 3)) + "," + sub.label());

// An instance that gains a property is at a new shape, and an argument's
// identity says nothing about how many properties it has.
const grown = new Pt(1, 1);
grown.extra = 9;
console.log(grown.dot(new Pt(2, 2)) + "," + grown.extra);

// A missing argument is `undefined`, which is what the call delivers and what
// the join has to widen to.
class Opt {
  need(p) {
    return p === undefined ? "none" : p.x;
  }
}
const op = new Opt();
console.log(op.need() + "," + op.need(new Pt(6, 0)));

// A parameter reassigned inside the body: the binding's type is what flows,
// not the signature's, from the assignment onward.
class Reassign {
  run(p) {
    let q = p;
    q = { x: "z" };
    return p.x + "," + q.x;
  }
}
console.log(new Reassign().run(new Pt(3, 0)));

// Warm past the point where every cache and every guard on the path is filled.
// Nothing poisons `mix`, so this is the case the mechanism exists for.
class Clean {
  constructor(v) {
    this.v = v;
  }
  mix(o) {
    return this.v * 10 + o.v;
  }
}
const c1 = new Clean(3);
const c2 = new Clean(4);
let hotSum = 0;
for (let i = 0; i < 500; i++) hotSum += c1.mix(c2);
console.log(hotSum);

// A poisoning call site that appears LATE — after the method, after a call that
// would have typed it. Whole-module ordering is the point: the first call is not
// allowed to have been believed.
class Late {
  use(p) {
    return p.x + 1;
  }
}
class Other {
  use(p) {
    return p.x + 2;
  }
}
const lt = new Late();
console.log(lt.use(new Pt(10, 0)));

function poisonLate(o) {
  return o.use(new Pt(20, 0));
}
console.log(poisonLate(lt) + "," + poisonLate(new Other()));
