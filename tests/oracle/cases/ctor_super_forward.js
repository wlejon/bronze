// A parameter carried down an `extends` chain by `super(...)`, through a link
// that declares no constructor of its own.
//
// The base constructor's call sites are not only its own `new` sites: every
// `super(...)` in a subclass is one, and so is every `new Sub(...)` when `Sub`
// declares no constructor, because the implicit one is
// `constructor(...args) { super(...args) }` and forwards positionally. Miss
// either road and the join speaks for callers it never saw.

class A {
  constructor(a = 0) {
    this.a = a;
  }
}

class B extends A {
  constructor(a, b = 0) {
    super(a);
    this.b = b;
  }
}

// No constructor: forwards both arguments to `B`, which forwards the first to
// `A`. Two implicit hops in a row is the shape three.js's `Line`/`LineSegments`
// pair has.
class C extends B {}
class C2 extends C {}

class D extends C2 {
  constructor(v) {
    super(v, v * 2);
  }
}

function sumAB(o) {
  return o.a + o.b;
}

const one = new B(1, 2);
console.log("b=" + one.a + "," + one.b + " " + sumAB(one));

const two = new C(3, 4);
console.log("c=" + two.a + "," + two.b + " " + sumAB(two));

const three = new C2(5, 6);
console.log("c2=" + three.a + "," + three.b + " " + sumAB(three));

const four = new D(7);
console.log("d=" + four.a + "," + four.b + " " + sumAB(four));

const bare = new A();
console.log("a=" + bare.a);
console.log("b defaults=" + new B(9).a + "," + new B(9).b);

// The chain's fields are ordinary slots on every rung, and the subclass's own
// write lands after the base's.
console.log("keys=" + Object.keys(four).join(",") + "/" + Object.keys(bare).join(","));
console.log("json=" + JSON.stringify(three));
console.log("proto=" + (four instanceof A) + (four instanceof B) + (four instanceof D));

let total = 0;
for (let i = 0; i < 100; i++) {
  total = total + sumAB(new D(i)) + sumAB(new C(i, 1));
}
console.log("loop=" + total);

four.a = 0.5;
four.b = four.a * 4;
console.log("mutated=" + sumAB(four));
