// A typed field with no initializer is a field all the same: defined as
// `undefined` when the instance is initialized, so its key takes its place in
// the property order there, before whatever the constructor assigns. node's
// transform keeps it (`a;`), and so must bronze.

class P {
  a: number;
  b: number;
  constructor() {
    this.b = 2;
    this.a = 1;
  }
}
console.log(JSON.stringify(new P()));

// A definite-assignment assertion and an optional field are fields too; a
// `declare` field is not.
class Q {
  c!: string;
  d?: number;
  declare e: number;
  constructor() {
    this.e = 5;
    this.c = "c";
  }
}
const q = new Q();
console.log(JSON.stringify(q), Object.keys(q).join(","));

// A parameter property is declared as a field ahead of the class's own, and
// assigned when the constructor starts.
class R {
  own = 1;
  late: number;
  constructor(public x: number, private y: number, readonly z: number) {
    this.late = 4;
  }
}
console.log(JSON.stringify(new R(1, 2, 3)));

// In a derived class: the parameter properties' fields and the class's own
// are defined when super() returns, then the properties are assigned.
class Base {
  constructor() {
    this.base = 0;
  }
  base: number;
}
class Derived extends Base {
  mine = "m";
  second: number;
  constructor(public first: number) {
    super();
    this.second = 2;
  }
}
console.log(JSON.stringify(new Derived(1)));

// The order holds over many instances, and the arithmetic over the typed
// fields is a number.
class V {
  x: number;
  y: number;
  constructor(x: number, y: number) {
    this.y = y;
    this.x = x;
  }
  sum(): number {
    return this.x + this.y;
  }
}
let total = 0;
for (let i = 0; i < 1000; i++) total += new V(i, 1).sum();
console.log(total, Object.keys(new V(1, 2)).join(","));
