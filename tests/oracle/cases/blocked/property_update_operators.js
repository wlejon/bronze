// BLOCKED: `unsupported construct: ++/-- on a property (write `o.k += 1`)`.
//
// Lowering's update path resolves its target as a BINDING — a local, or a slot
// in an enclosing environment record — and a MemberExpression is neither.
// `o.k += 1` already works, so the missing piece is not the read-modify-write
// but the reference: `o.k++` has to evaluate the base ONCE and then read and
// write through it, which is exactly what `lowerCompoundMemberAssign` does and
// exactly what this path cannot currently reach. `a[i]++` adds the second half
// of the same problem, since the index expression must not be evaluated twice
// either — `a[i++]++` would otherwise increment `i` twice.
//
// It is here rather than in `cases/` because docs/0022 found it while writing
// an iterator (`this.i++` is how every one of them is spelled) and chose to
// name it rather than build it inside a chunk about builtins: it is a lowering
// change with an evaluation-order contract of its own, and an unpinned
// evaluation order is how docs/0000's "plausible but wrong" bugs got in.
//
// What this case pins when it lands, from ECMA-262 13.4 (update expressions)
// and 13.15.2 (the reference evaluation they share with compound assignment):
//
// 1. Postfix yields the OLD value and prefix the new one, and the write has
//    already happened by the time the next argument is evaluated.
// 2. The base and the index of `a[i]++` are each evaluated exactly once.
// 3. `this.k++` inside a method updates the receiver, which is the reason the
//    construct is worth its own chunk.
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
