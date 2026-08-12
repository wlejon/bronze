// A derived class with no constructor of its own.
//
// ECMA-262 15.7.14 gives it `constructor(...args) { super(...args); }`, and
// the forwarding has to be exact: `Mid` passes on however many arguments it
// was given, not a fixed two, which is what `CountChild` pins by counting
// them. A chain of two implicit constructors forwards through both, and a
// derived class WITH a constructor is unaffected.
//
// The receiver matters as much as the arguments: the parent constructor runs on
// the instance being built, so the properties it installs and the methods on
// its prototype are visible on a `Mid`.
class Base {
  constructor(a, b) {
    this.sum = a + b;
  }
  total() {
    return this.sum;
  }
}
class Mid extends Base {}
class Leaf extends Mid {}
console.log(new Mid(1, 2).total());
console.log(new Leaf(3, 4).total());
console.log(new Mid(1, 2).sum);
class Count {
  constructor(...items) {
    this.n = items.length;
  }
}
class CountChild extends Count {}
console.log(new CountChild(1, 2, 3).n);
console.log(new CountChild().n);
console.log(new CountChild(9).n);
class Explicit extends Base {
  constructor(a) {
    super(a, 100);
  }
  total() {
    return super.total() + 1;
  }
}
console.log(new Explicit(5).total());
class DefaultedBase {
  constructor(v = 7) {
    this.v = v;
  }
}
class DefaultedChild extends DefaultedBase {}
console.log(new DefaultedChild().v);
console.log(new DefaultedChild(1).v);
