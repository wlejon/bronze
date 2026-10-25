// What the constructor sees of its own instance before it assigns a declared
// field: the key is already there, holding undefined.

class Base {
  a;
  b;
  constructor() {
    console.log("base", JSON.stringify(Object.keys(this)), this.a, "a" in this);
    this.a = 1;
    console.log("base after", JSON.stringify(Object.keys(this)), this.a);
    this.b = 2;
  }
}
new Base();

// A derived class's fields appear when super() returns, after the base's.
class Derived extends Base {
  c;
  d = 4;
  constructor() {
    super();
    console.log("derived", JSON.stringify(Object.keys(this)), this.c, this.d);
    this.c = 3;
  }
}
console.log(JSON.stringify(new Derived()));

// A method the base constructor calls sees the base's fields but not the
// derived class's, which are defined only once super() has returned.
class Early {
  e;
  constructor() {
    this.show();
    this.e = "e";
  }
  show() {
    console.log("show", JSON.stringify(Object.keys(this)), this.e);
  }
}
class Late extends Early {
  l;
  show() {
    console.log("override", JSON.stringify(Object.keys(this)), "l" in this);
  }
}
new Early();
new Late();

// A later field's initializer can read an earlier field before the
// constructor assigns it.
class Reader {
  x;
  y = this.x;
  constructor() {
    this.x = 10;
  }
}
console.log(JSON.stringify(new Reader()));

// A numeric field read in a loop after construction, so the read that sees
// undefined above and the reads that see numbers here are one program.
class Num {
  n;
  constructor(v) {
    this.n = v;
  }
}
let s = 0;
for (let i = 0; i < 100; i++) s += new Num(i).n;
console.log(s);
