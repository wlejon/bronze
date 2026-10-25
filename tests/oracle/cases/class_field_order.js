// A field declared without an initializer is defined as `undefined` when the
// instance is initialized, so its key takes its place in the property order
// there — before anything the constructor assigns, whatever order the
// constructor assigns in.

class P {
  a;
  b;
  constructor() {
    this.b = 2;
    this.a = 1;
  }
}
console.log(JSON.stringify(new P()));
console.log(Object.keys(new P()).join(","));

// Undeclared keys follow the declared ones, in assignment order.
class Q {
  z;
  constructor() {
    this.y = 1;
    this.x = 2;
    this.z = 3;
  }
}
console.log(JSON.stringify(new Q()));

// A field with an initializer and one without, interleaved.
class R {
  c = 3;
  a;
  b = 2;
  constructor() {
    this.d = 4;
    this.a = 1;
  }
}
console.log(JSON.stringify(new R()));

// In a derived class the fields are defined when super() returns, after the
// base's own keys and before the rest of the constructor's.
class Base {
  constructor() {
    this.base = 0;
  }
}
class Derived extends Base {
  second;
  first;
  constructor() {
    super();
    this.later = 9;
    this.first = 1;
    this.second = 2;
  }
}
console.log(JSON.stringify(new Derived()));

// A field the constructor never assigns stays, as undefined.
class S {
  kept;
  constructor() {
    this.other = 1;
  }
}
const s = new S();
console.log(JSON.stringify(Object.keys(s)), "kept" in s, s.kept);

// The same class many times: the order is the same for every instance, and
// a numeric read of the assigned field is still a number.
class V {
  x;
  y;
  constructor(x, y) {
    this.y = y;
    this.x = x;
  }
  sum() {
    return this.x + this.y;
  }
}
let total = 0;
for (let i = 0; i < 1000; i++) total += new V(i, 1).sum();
console.log(total, Object.keys(new V(1, 2)).join(","));
