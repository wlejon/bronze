// Method dispatch soundness (chunk 24): a class method's parameter types are a
// join over the call sites that can reach it, and the body is compiled on that
// join. Every caller below the first block passes numbers, so this probe is the
// one site that reaches `set` with something else, through: this reparent.
// A site that wrongly bounds its dispatch leaves `x` typed number and prints
// NaN (or a wrong typeof) where node prints the string.
class A {
  constructor() { this.x = 0; this.t = ''; }
  go(k, p, v) { this[k] = p; return this.set(v); }
  set(x) { this.x = x + 1; this.t = typeof x; return this; }
}
class B {
  constructor() { this.x = 0; this.t = ''; }
  set(x) { this.x = x * 3; this.t = 'b' + typeof x; return this; }
}
const a = new A(), b = new B();
for (let i = 0; i < 5; i++) { a.set(i); b.set(i); }
a.go('y', 1, 2);
console.log(a.x, a.t);
const key = ['__pro', 'to__'].join('');
const a2 = new A();
a2.go(key, B.prototype, 's');
console.log(a2.x, a2.t);
