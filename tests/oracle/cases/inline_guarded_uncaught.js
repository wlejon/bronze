// An exception thrown three method bodies deep, each compiled in place at its
// call site (lower/inline_guarded.cpp), and caught by nothing: the program
// stops at it with the same report, frames and exit status as if every call
// had been made.

class C { leaf(i) { if (i === 50) throw new TypeError('at ' + i); return i; } }
class B { constructor() { this.c = new C(); } mid(i) { return this.c.leaf(i) + 1; } }
class A { constructor() { this.b = new B(); } top(i) { return this.b.mid(i) * 2; } }

function run(a, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += a.top(i);
  return s;
}
const a = new A();
console.log(run(a, 50));
console.log(run(a, 100));
console.log('not reached');
