// Method dispatch soundness (chunk 24): a class method's parameter types are a
// join over the call sites that can reach it, and the body is compiled on that
// join. Every caller below the first block passes numbers, so this probe is the
// one site that reaches `set` with something else, through: getproto this.
// A site that wrongly bounds its dispatch leaves `x` typed number and prints
// NaN (or a wrong typeof) where node prints the string.
class A {
  constructor() { this.x = 0; this.t = ''; }
  go(v) { return this.set(v); }
  set(x) { this.x = x + 1; this.t = typeof x; return this; }
}
class B {
  constructor() { this.x = 0; this.t = ''; }
  set(x) { this.x = x * 3; this.t = 'b' + typeof x; return this; }
}
class C extends A { set(x) { return super.set(x); } }
const a = new A(), b = new B(), c = new C();
for (let i = 0; i < 5; i++) { a.set(i); b.set(i); c.set(i); }
const key = ['__pro', 'to__'].join('');
const pb = Object.getPrototypeOf(b);
const m = new Map([[key, A.prototype]]);
for (const [k, v] of m) pb[k] = v;
b.go('s');
console.log(b.x, b.t);
