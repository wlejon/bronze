// Method dispatch soundness (chunk 24): a class method's parameter types are a
// join over the call sites that can reach it, and the body is compiled on that
// join. Every caller below the first block passes numbers, so this probe is the
// one site that reaches `set` with something else, through: mixin this.
// A site that wrongly bounds its dispatch leaves `x` typed number and prints
// NaN (or a wrong typeof) where node prints the string.
class A {
  constructor() { this.x = 0; this.t = ''; }
  go(v) { return this.set(v); }
  set(x) { this.x = x + 1; this.t = typeof x; return this; }
}
const M = (Base) => class extends Base { set(x) { this.x = x * 2; this.t = 'm' + typeof x; return this; } };
const MA = M(A);
const ma = new MA();
for (let i = 0; i < 5; i++) ma.set(i);
console.log(ma.x, ma.t);
ma.go('s');
console.log(ma.x, ma.t);
