// Method dispatch soundness (chunk 24): a class method's parameter types are a
// join over the call sites that can reach it, and the body is compiled on that
// join. Every caller below the first block passes numbers, so this probe is the
// one site that reaches `set` with something else, through: fn base this.
// A site that wrongly bounds its dispatch leaves `x` typed number and prints
// NaN (or a wrong typeof) where node prints the string.
class A {
  constructor() { this.x = 0; this.t = ''; }
  go(v) { return this.set(v); }
  set(x) { this.x = x + 1; this.t = typeof x; return this; }
}
function F() { this.x = 0; this.t = ''; }
F.prototype = Object.create(A.prototype);
class D extends F { set(x) { this.x = x * 2; this.t = 'd' + typeof x; return this; } }
const d = new D();
for (let i = 0; i < 5; i++) d.set(i);
console.log(d.x, d.t);
d.go('s');
console.log(d.x, d.t);
