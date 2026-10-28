// Method calls compiled in place (lower/inline_guarded.cpp) stop at a depth
// and never copy a method into itself: recursive and mutually recursive
// methods keep real calls past the copies, and a recursion that runs out of
// stack still throws a RangeError the caller catches.

class Node {
  constructor(depth) { this.v = depth; this.next = depth > 0 ? new Node(depth - 1) : null; }
  sum() { return this.next === null ? this.v : this.v + this.next.sum(); }
  ping(n) { return n <= 0 ? 0 : 1 + this.pong(n - 1); }
  pong(n) { return n <= 0 ? 0 : 2 + this.ping(n - 1); }
  a(n) { return this.b(n) + 1; }
  b(n) { return this.c(n) + 1; }
  c(n) { return this.d(n) + 1; }
  d(n) { return this.e(n) + 1; }
  e(n) { return n; }
  forever(n) { return this.forever(n + 1) + 1; }
  throwsDeep(n) { if (n === 0) throw new RangeError('bottom'); return this.throwsDeep(n - 1); }
}
class Holder { constructor() { this.n = new Node(40); } }

function run(h, k) {
  let s = 0;
  for (let i = 0; i < k; i++) {
    s += h.n.sum();
    s += h.n.ping(i % 9);
    s += h.n.a(i);
  }
  return s;
}

const h = new Holder();
console.log(run(h, 200));

function overflow(h, k) {
  let caught = 0;
  for (let i = 0; i < k; i++) {
    try { h.n.forever(0); } catch (e) { if (e instanceof RangeError) caught++; }
  }
  return caught;
}
console.log(overflow(h, 2));

function deep(h, k) {
  const out = [];
  for (let i = 0; i < k; i++) {
    try { h.n.throwsDeep(i * 10); } catch (e) { out.push(e.name + ':' + e.message); }
  }
  return out.join(' ');
}
console.log(deep(h, 4));
