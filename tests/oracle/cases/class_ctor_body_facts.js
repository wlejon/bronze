// A class constructor's body is COPIED by lowering, so that the field
// initializers can be spliced into it without editing the tree inference read.
// Every proof inference records is keyed on node identity, so the copy decides
// whether a constructor is compiled with what was proven about it or with
// nothing at all — and, if the pairing between copy and original is ever wrong,
// with a proof about some OTHER piece of code.
//
// Every line below is a way for that pairing to be wrong: a binding whose type
// changes between the class definition and the construction, a loop counter
// that stops being a number inside the constructor, a receiver that joins two
// classes, a field initializer reading a free name, a class declared inside
// another class's constructor (a copy of a copy), and a derived constructor
// whose fields run after `super`. Each has to answer exactly what it answered
// when the copy carried no facts at all.

class V {
  constructor(x) {
    this.x = x;
    this.y = 0;
  }
  add(o) {
    this.x += o.x;
    return this;
  }
}

// A local bound to a `new`, written and read inside the constructor. The plain
// case the copy was losing: proven identity, proven slot.
class Simple {
  constructor() {
    const v = new V(1);
    v.x = 5;
    v.y = 6;
    this.total = v.x + v.y;
  }
}
console.log(new Simple().total);

// A loop counter inside a constructor. The loop's merge point is a STATEMENT,
// and its recorded binding types are what give the header block its parameter
// types — so a mispaired statement types the header from another loop.
class Loop {
  constructor(n) {
    let sum = 0;
    for (let i = 0; i < n; i++) {
      sum += i;
    }
    // The same loop again, with a counter that stops being a number. The
    // header has to widen, and a proof taken from the loop above would unbox
    // a string.
    let j = 0;
    let acc = "";
    while (j !== "done") {
      acc += j;
      j = j + 1;
      if (j === 3) j = "done";
    }
    this.sum = sum;
    this.acc = acc;
  }
}
const lp = new Loop(4);
console.log(lp.sum + "," + lp.acc);

// A binding that holds two different classes: the join keeps the kind and
// loses the identity, and the property sites stay polymorphic.
class W {
  constructor() {
    this.x = "w";
  }
}
class Joined {
  constructor(flag) {
    const o = flag ? new V(2) : new W();
    this.got = o.x;
  }
}
console.log(new Joined(true).got + "," + new Joined(false).got);

// A field initializer reads a FREE NAME. Inference walks the initializer in the
// scope that holds the class declaration; lowering copies it into the
// constructor, which runs later and can see a different value. The copy is
// therefore made with no facts of its own, and this is the line that says so:
// `outer` is a number when the class is defined and a string at every
// construction.
let outer = 1;
class Field {
  v = outer;
  constructor() {
    this.doubled = this.v + this.v;
  }
}
outer = "s";
const f1 = new Field();
console.log(f1.v + "," + f1.doubled);

// A derived constructor: the fields run after `super(...)`, in the middle of a
// copied statement list, and the statements on both sides of them keep their
// own facts.
class Base {
  constructor(a) {
    this.a = a;
  }
}
class Derived extends Base {
  b = 10;
  constructor(a) {
    const before = new V(a);
    super(before.x);
    const after = new V(this.b);
    this.c = before.x + after.x;
  }
}
const d = new Derived(3);
console.log(d.a + "," + d.b + "," + d.c);

// A class declared INSIDE another class's constructor. Its own constructor is
// a copy of a copy, so the translation from the node lowering holds back to the
// node inference walked is a chain rather than one step.
class Outer {
  constructor() {
    class Inner {
      constructor() {
        const v = new V(7);
        v.x = v.x + 1;
        this.n = v.x;
      }
    }
    this.inner = new Inner().n;
  }
}
console.log(new Outer().inner);

// A constructor that is never entered by any `new` in this program still has to
// lower — dead code is compiled, exported and verified like any other.
class Unused {
  constructor(q) {
    const v = new V(q);
    this.v = v.x;
  }
}

// `this` inside a constructor of a class somebody extends: the runtime shape is
// the SUBCLASS's, so the guard has to tolerate a family of shapes rather than
// pin one. Both are constructed, in that order and then the reverse.
class Shape2 {
  constructor(k) {
    this.k = k;
    this.tag = "shape";
  }
  read() {
    return this.k + this.tag;
  }
}
class Circle extends Shape2 {
  constructor(k) {
    super(k);
    this.r = 2;
  }
}
let hot = "";
for (let i = 0; i < 200; i++) {
  hot = new Circle(i).read();
  hot = new Shape2(i).read() + hot;
}
console.log(hot);

// A constructor whose local is reassigned to a value of another kind after the
// property reads that were proven. The proof holds at the first read and must
// not be carried past the write.
class Reassigned {
  constructor() {
    let p = new V(4);
    const first = p.x;
    p = { x: "str" };
    this.out = first + "," + p.x;
  }
}
console.log(new Reassigned().out);

// A constructor that throws mid-body: the statements before the throw ran and
// their effects stand.
let sideEffect = 0;
class Thrower {
  constructor() {
    const v = new V(9);
    sideEffect = v.x;
    throw new Error("boom");
  }
}
try {
  new Thrower();
} catch (e) {
  console.log(sideEffect + "," + e.message);
}

// Chained method calls on a constructor local, warmed past the point where
// every cache and guard on the path is filled.
class Warm {
  constructor() {
    const a = new V(0);
    const b = new V(1);
    for (let i = 0; i < 300; i++) {
      a.add(b);
    }
    this.x = a.x;
  }
}
console.log(new Warm().x);
