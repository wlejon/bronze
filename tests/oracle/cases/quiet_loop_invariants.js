// What a call-free loop asks once in front of itself: every result must match node.
const out = [];
class P { constructor(x, y) { this.x = x; this.y = y; }
  sum() { return this.x + this.y; }
  bump(d) { this.x = this.x + d; return this; }
  dot(o) { return this.x * o.x + this.y * o.y; } }
const a = new P(1.5, 2.5), b = new P(3, 4);

// 1. reads of an object the loop does not change
function reads(o, n) { let s = 0; for (let i = 0; i < n; i++) s += o.x * i + o.y; return s; }
out.push(reads(a, 20000), reads(b, 20000), reads(a, 0), reads(new P(7, 8), 3));

// 2. the loop stores the slot it reads, through the same object
function storesSame(o, n) { let s = 0; for (let i = 0; i < n; i++) { o.x = o.x + 1; s += o.x + o.y; } return s; }
out.push(storesSame(new P(0, 1), 20000), storesSame(new P(0.5, 1), 5));

// 3. ... and through another name for it
function storesAlias(p, q, n) { let s = 0; for (let i = 0; i < n; i++) { p.x = i; s += q.x + q.y; } return s; }
const al = new P(1, 2);
out.push(storesAlias(new P(1, 2), new P(5, 6), 20000), storesAlias(al, al, 20000), storesAlias(al, new P(9, 9), 3));

// 4. inlined methods on a receiver the loop does not define
function methods(o, p, n) { let s = 0; for (let i = 0; i < n; i++) s += o.dot(p) + o.sum() + i; return s; }
out.push(methods(a, b, 20000), methods(b, a, 20000));
function bumps(o, n) { let s = 0; for (let i = 0; i < n; i++) s += o.bump(0.25).sum(); return s; }
out.push(bumps(new P(0, 0), 20000), bumps(new P(1, 1), 7));

// 5. a receiver that is sometimes nothing, behind its own test
function maybe(o, n) { let s = 0; for (let i = 0; i < n; i++) { if (o) s += o.x; else s -= 1; } return s; }
out.push(maybe(a, 20000), maybe(null, 20000), maybe(undefined, 5), maybe(a, 20000), maybe(0, 3));

// 6. a receiver of another shape, then the first again
const odd = { y: 10, x: 20 }, getter = { get x() { return ++getter.n; }, n: 0, y: 1 };
out.push(reads(odd, 20000), reads(getter, 50), getter.n, reads(a, 20000), reads({ x: 'a', y: 'b' }, 3));

// 7. a captured variable the loop's inlined callee writes
let counter = 0;
function tick() { counter = counter + 1; return counter; }
function ticks(n) { let s = 0; for (let i = 0; i < n; i++) s += tick() + counter; return s; }
out.push(ticks(20000), counter, ticks(20000), counter);
let scale = 2;
function scaled(o, n) { let s = 0; for (let i = 0; i < n; i++) s += o.x * scale; return s; }
out.push(scaled(a, 20000)); scale = 3; out.push(scaled(a, 20000)); scale = 'x'; out.push(scaled(a, 2));

// 8. the method is replaced between two runs of the loop, and the shape between two
out.push(methods(a, b, 20000));
P.prototype.sum = function () { return 100; };
out.push(methods(a, b, 20000));
a.z = 9; delete a.x;
out.push(methods(a, b, 5), reads(a, 5), methods(b, a, 5));

// 9. a loop inside a loop: the inner reads the outer's object
function nested(rows, n) { let s = 0; for (const r of rows) { for (let i = 0; i < n; i++) s += r.x + r.y * i; } return s; }
out.push(nested([new P(1, 2), new P(3, 4), { x: 1, y: 1 }, new P(5, 6)], 5000));

// 10. the class's constructor, made and dropped, with an operand that changes shape later
const k = new P(2, 3);
function temps(o, n) { let s = 0; for (let i = 0; i < n; i++) s += new P(i, 1).dot(o); return s; }
out.push(temps(k, 20000)); k.w = 1; out.push(temps(k, 20000)); out.push(temps({ x: 1, y: 1 }, 20000));

console.log(out.join('\n'));
