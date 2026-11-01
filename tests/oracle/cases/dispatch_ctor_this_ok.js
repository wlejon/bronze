// Method dispatch soundness (chunk 24): a class method's parameter types are a
// join over the call sites that can reach it, and the body is compiled on that
// join. Every caller below the first block passes numbers, so this probe is the
// one site that reaches `set` with something else, through: ctor this ok.
// A site that wrongly bounds its dispatch leaves `x` typed number and prints
// NaN (or a wrong typeof) where node prints the string.
class A {
  constructor() { this.x = 0; this.t = ''; }
  set(x) { this.x = x + 1; this.t = typeof x; return this; }
}
class B {
  constructor() { this.x = 0; this.t = ''; }
  set(x) { this.x = x + 1; this.t = typeof x; return this; }
}
function hotA(v, n) { for (let i = 0; i < n; i++) v.set(i); return v.x; }
function hotB(v, n) { for (let i = 0; i < n; i++) v.set(i); return v.x; }
const a = new A(), b = new B();
console.log(hotA(a, 10), hotB(b, 10));
class P { constructor(v) { this.x = 0; this.set(v); } set(x) { this.x = x + 1; return this; } } let s = 0; for (let i = 0; i < 5; i++) s += new P(i).x; console.log(s);
