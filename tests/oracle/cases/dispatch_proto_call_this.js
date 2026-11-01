// Method dispatch soundness (chunk 24): a class method's parameter types are a
// join over the call sites that can reach it, and the body is compiled on that
// join. Every caller below the first block passes numbers, so this probe is the
// one site that reaches `set` with something else, through: proto call this.
// A site that wrongly bounds its dispatch leaves `x` typed number and prints
// NaN (or a wrong typeof) where node prints the string.
class A {
  constructor() { this.x = 0; this.t = ''; }
  fill(k, v) { this[k] = v; return this; }
  go(v) { return this.set(v); }
  set(x) { this.x = x + 1; this.t = typeof x; return this; }
}
class B {
  constructor() { this.x = 0; this.t = ''; }
  set(x) { this.x = x * 3; this.t = 'b' + typeof x; return this; }
}
const a = new A(), b = new B();
for (let i = 0; i < 5; i++) { a.set(i); b.set(i); }
const key = ['__pro', 'to__'].join('');
A.prototype.fill(key, B.prototype);
const a2 = new A();
a2.go('s');
console.log(a2.x, a2.t);
