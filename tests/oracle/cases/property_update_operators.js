// `o.k++`, `++o.k` and `a[i]--` — an update expression whose target is a
// PROPERTY rather than a variable (ECMA-262 13.4).
//
// Lowering's update path resolved its target as a BINDING — a local, or a
// slot in an enclosing environment record — and a MemberExpression is
// neither, so `this.version ++` was a named hard error. What was missing was
// never the read-modify-write; it was the REFERENCE: `o.k++` must evaluate
// the base once and then read and write through that one value, which is the
// evaluation-order contract compound assignment already signed (13.15.2) and
// which `a[i]++` extends to the index — lowering `i` twice would make
// `a[i++]++` increment `i` twice.
//
// What this pins, from ECMA-262 13.4 (update expressions) and 13.15.2 (the
// reference evaluation they share with compound assignment):
//
// 1. Postfix yields the OLD value and prefix the new one, and the write has
//    already happened by the time the next argument is evaluated — which is
//    why `console.log(o.n++, o.n)` prints two different numbers.
// 2. The base and the index of `a[i]++` are each evaluated exactly once.
// 3. `this.k++` inside a method updates the receiver, which is the reason the
//    construct is worth its own lowering unit.

const o = { n: 1 };
o.n++;
console.log(o.n);
console.log(o.n++, o.n);
console.log(++o.n, o.n);

const a = [10, 20];
let i = 0;
a[i]++;
console.log(a[0]);
a[i++] -= 1;
console.log(a[0], i);
console.log(a[1]--, a[1]);

class C {
  constructor() { this.k = 0; }
  bump() { return this.k++; }
}
const c = new C();
console.log(c.bump(), c.bump(), c.k);
