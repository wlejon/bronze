// Classes past what class_basics pins: an inherited method that is NOT
// overridden, a static reached through a subclass, the default constructor
// of a base class that writes none, one function object shared by every
// instance, a class declared in a function scope closing over that scope,
// and a static and an instance method sharing a name (they live on
// different objects, so they do not collide).
class Shape {
  constructor(name) { this.name = name; }
  describe() { return "a " + this.name; }
  static kind() { return "shape"; }
}
class Circle extends Shape {
  constructor(r) { super("circle"); this.r = r; }
  area() { return 3 * this.r * this.r; }
}
const c = new Circle(2);
console.log(c.describe());
console.log(c.area());
console.log(Circle.kind());
console.log(Shape.kind());

class Empty {}
const e = new Empty();
e.tag = "set later";
console.log(e.tag);

const a = new Circle(1);
const b = new Circle(1);
console.log(a.area === b.area);

function makeCounter(step) {
  class Counter {
    constructor() { this.n = 0; }
    bump() { this.n = this.n + step; return this.n; }
  }
  return new Counter();
}
const k = makeCounter(5);
k.bump();
console.log(k.bump());

class Both {
  constructor() { this.v = 1; }
  tag() { return "instance"; }
  static tag() { return "static"; }
}
console.log(new Both().tag() + "/" + Both.tag());
