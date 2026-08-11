// Destructuring as an ASSIGNMENT rather than a declaration (docs/0017
// decision 5): the same patterns, writing bindings that already exist.
//
// The swaps are the point. `[a, b] = [b, a]` is a swap only if the whole
// right side is evaluated before any target is written (ECMA-262 13.15.5
// evaluates the initializer, then destructures it), and the three-way
// rotation would come out wrong under any left-to-right read-and-write.
//
// `({ x, y } = o)` needs the parentheses because a statement starting with
// `{` is a block; that is a grammar fact, and this line pins that bronze
// reads the parenthesized form as the assignment it is.
//
// The value of the assignment EXPRESSION is the right-hand side, not the
// pattern, which `whole` pins. And `counter` is read by a closure, so it
// lives in an environment record — a pattern write must reach the same
// binding an ordinary `counter =` would, or `bump` would not see it.
let a = 1;
let b = 2;
[a, b] = [b, a];
console.log(a + "," + b);
let p = 1;
let q = 2;
let r = 3;
[p, q, r] = [r, p, q];
console.log(p + "," + q + "," + r);
let x = 0;
let y = 0;
({ x, y } = { x: 5, y: 6 });
console.log(x + y);
let m = 0;
let deep = 0;
[m, [deep]] = [1, [2]];
console.log(m + deep);
let d1 = 0;
({ d1 = 8 } = {});
console.log(d1);
let d2 = 0;
({ d2 = 8 } = { d2: null });
console.log(d2);
let first = 0;
let others = 0;
[first, ...others] = [1, 2, 3];
console.log(first + ":" + others.length);
let taken = 0;
const whole = ([taken] = [7, 8]);
console.log(taken + ":" + whole.length);
let counter = 0;
function bump() { counter = counter + 1; return counter; }
[counter] = [10];
console.log(bump());
let renamed = 0;
({ k: renamed } = { k: 42 });
console.log(renamed);
