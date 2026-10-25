// The constructor of a class with typed fields sees their keys, holding
// undefined, before it assigns them; node's transform keeps every one.

class Point {
  x: number;
  y: number;
  constructor(x: number, y: number) {
    console.log("before", JSON.stringify(Object.keys(this)), this.x);
    this.y = y;
    this.x = x;
    console.log("after", JSON.stringify(Object.keys(this)), this.x + this.y);
  }
}
const p = new Point(1, 2);
console.log(JSON.stringify(p));

// A parameter property's key is there right after super() returns, and its
// value is assigned straight after.
class Base {
  constructor() {
    console.log("base", JSON.stringify(Object.keys(this)));
  }
}
class Derived extends Base {
  z: number = 0;
  constructor(public w: number) {
    super();
    console.log("derived", JSON.stringify(Object.keys(this)), this.w);
  }
}
console.log(JSON.stringify(new Derived(7)));

// Reading a typed field before its assignment, in a loop, alongside reads
// after it.
class Acc {
  total: number;
  seen: string;
  constructor(v: number) {
    this.seen = typeof this.total;
    this.total = v;
  }
}
let sum = 0;
let kinds = "";
for (let i = 0; i < 50; i++) {
  const a = new Acc(i);
  sum += a.total;
  if (i === 0) kinds = a.seen;
}
console.log(sum, kinds);
