// The four roads to a constructor that do not go through `new C(...)`, and the
// poison each one owes.
//
// A class whose binding is only ever the callee of a `new` has enumerable
// construction sites. One that is READ as a value does not: whoever holds the
// value can construct it with anything, and this compilation cannot see with
// what. A module `const` holding the class is one road, an argument is a
// second, `.bind` — which makes a constructor whose leading arguments were
// decided elsewhere — is a third, and a call without `new` is a fourth: a
// TypeError on a class, but a perfectly ordinary call on anything else, so the
// poison has to exist whether or not a real library ever writes one.
//
// `Reflect.construct` is the fifth and is not exercised here, because bronze
// does not implement it yet (`typeof Reflect.construct` is `undefined`); the
// class it names is read as a value at the call, so the same poison covers it.
//
// Each of those classes devolves to the boxed path and keeps meaning what the
// language says. `Free`, next to them in the same program, does not.

class Held {
  constructor(h = 0) {
    this.h = h;
  }
}

class Bnd {
  constructor(m = 0) {
    this.m = m;
  }
}

class Called {
  constructor(c = 0) {
    this.c = c;
  }
}

class Passed {
  constructor(g = 0) {
    this.g = g;
  }
}

class Free {
  constructor(f = 0) {
    this.f = f;
  }
}

function build(Ctor, arg) {
  return new Ctor(arg);
}

function freeF(v) {
  return v.f;
}

// The binding a `new` goes through is one this compilation can follow to a
// class — and following it is exactly what makes the class's sites unknowable,
// because the value it found could have gone anywhere else too.
const HeldCtor = Held;
const h1 = new HeldCtor(5);
const h2 = new HeldCtor("five");
console.log("held=" + h1.h + "," + h2.h + " " + (h1.h + 1) + " " + (h2.h + "!"));
console.log("held direct=" + new Held(2).h + "," + new Held().h);

const BoundBnd = Bnd.bind(null);
const b1 = new BoundBnd(7);
const b2 = new BoundBnd("seven");
console.log("bound=" + b1.m + "," + b2.m + " " + (b1.m * 2) + " " + b2.m.length);
console.log("bound direct=" + new Bnd(3).m);

try {
  Called(1);
} catch (e) {
  console.log("plain call: " + (e instanceof TypeError));
}
console.log("called=" + new Called(4).c + "," + new Called().c);

console.log("passed=" + build(Passed, 8).g + "," + build(Passed, "eight").g);
console.log("passed direct=" + new Passed(1).g);

console.log("free=" + freeF(new Free(1.5)) + "," + freeF(new Free()) + "," + new Free(2).f);
const free = new Free(3);
free.f = free.f + 0.5;
console.log("free mutated=" + freeF(free));

let sum = 0;
for (let i = 0; i < 40; i++) {
  sum = sum + freeF(new Free(i)) + new Held(i).h + build(Passed, i).g;
}
console.log("loop=" + sum);

console.log("json=" + JSON.stringify(free) + JSON.stringify(h1) + JSON.stringify(b1));
console.log("keys=" + Object.keys(b2).join(",") + "/" + Object.keys(free).join(","));
