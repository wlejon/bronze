// What a method compiled in place at its call site (lower/inline_guarded.cpp)
// sees of the call: `this`, the arguments passed and `undefined` for the ones
// not, defaults and rest parameters, `arguments`, extra arguments evaluated
// in order and dropped, and a return of nothing. Methods that read their
// activation (arguments, rest, a closure) keep the real call; each prints the
// same either way.

const order = [];
function tag(v) { order.push(v); return v; }

class M {
  constructor() { this.k = 10; }
  pair(a, b) { return [this.k, a, b === undefined ? 'u' : b].join('/'); }
  dflt(a, b = a * 2, c = this.k) { return a + b + c; }
  rest(a, ...more) { return a + ':' + more.length; }
  args() { return arguments.length + ':' + Array.prototype.slice.call(arguments).join(','); }
  noret(v) { this.k += v; }
  self() { return this; }
  closes(v) { return () => this.k + v; }
  numeric(x, y) { return x * y + this.k; }
  // The same shapes with a call inside, which the leaf inliner leaves alone.
  dfltCall(a, b = a * 2, c = this.k) { return String(a + b + c); }
  numCall(x, y) { return Math.max(x * y, this.k); }
  noretCall(v) { this.k = Math.min(this.k + v, 1000); }
  selfCall() { tag('self'); return this; }
}
class Hold { constructor() { this.m = new M(); } }

function run(h, n) {
  const out = [];
  for (let i = 0; i < n; i++) {
    out.push(h.m.pair(i));
    out.push(h.m.pair(i, 'b'));
    out.push(h.m.pair(tag('x' + i), tag('y' + i), tag('z' + i)));
    out.push(h.m.dflt(i));
    out.push(h.m.dflt(i, 1));
    out.push(h.m.dflt(i, undefined, 0));
    out.push(h.m.rest(i, 1, 2, 3));
    out.push(h.m.args(i, 'a'));
    out.push(String(h.m.noret(1)));
    out.push(h.m.self() === h.m);
    out.push(h.m.closes(i)());
    out.push(h.m.numeric(i, 0.5));
    out.push(h.m.numeric('3', i));
  }
  return out.join(' ');
}

const h = new Hold();
for (let w = 0; w < 50; w++) run(h, 4);
order.length = 0;
console.log(run(h, 3));
console.log(order.join(','));

// The same calls made from a method, on a receiver the site reads from `this`.
class Driver {
  constructor(m) { this.m = m; }
  go(n) {
    const out = [];
    for (let i = 0; i < n; i++) {
      out.push(this.m.dflt(i));
      out.push(this.m.dflt(i, 1));
      out.push(this.m.numeric(i, 0.25));
      out.push(this.m.numeric(undefined, i));
      out.push(String(this.m.noret(-1)));
      out.push(this.m.self() === this.m);
      out.push(this.m.pair());
      out.push(this.m.dfltCall(i));
      out.push(this.m.dfltCall(i, 1));
      out.push(this.m.dfltCall(i, undefined, -1));
      out.push(this.m.numCall(i, 100));
      out.push(this.m.numCall('2', i));
      out.push(String(this.m.noretCall(2)));
      out.push(this.m.selfCall() === this.m);
    }
    return out.join(' ');
  }
}
const d = new Driver(new M());
for (let w = 0; w < 50; w++) d.go(3);
order.length = 0;
console.log(d.go(3));
console.log(order.length);
