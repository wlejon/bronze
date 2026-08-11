// Classes. `class` is not even a keyword today, so this is a parse error.
// A class is the constructor function plus its prototype (docs/0008), so
// what has to work is: methods shared by every instance, `new`, a static
// member on the constructor itself, extends wiring the prototype chain,
// super(...) running the parent constructor on the same instance, and
// super.method() reaching past an override.
class Point {
  constructor(x, y) {
    this.x = x;
    this.y = y;
  }
  sum() { return this.x + this.y; }
  scale(k) { return new Point(this.x * k, this.y * k); }
  static origin() { return new Point(0, 0); }
}
const p = new Point(1, 2);
console.log(p.sum());
console.log(p.scale(3).sum());
console.log(Point.origin().sum());

class Base {
  constructor(kind) { this.kind = kind; }
  describe() { return "I am " + this.kind; }
}
class Derived extends Base {
  constructor() {
    super("derived");
    this.extra = 1;
  }
  describe() { return super.describe() + "!"; }
}
const d = new Derived();
console.log(d.describe());
console.log(d.extra);
console.log(d.kind);
