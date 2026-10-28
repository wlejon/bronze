// Method calls in a loop whose site resolves to one class method are compiled
// in place behind the site's method cache (lower/inline_guarded.cpp). The
// frames of a body compiled that way are still reported: Error.stack names
// every inlined callee, innermost first, each at its own line and column, and
// then the caller, as if each call had been made.

function frames(stack) {
  const out = [];
  for (const line of stack.split('\n').slice(1)) {
    const m = line.match(/at (?:(\S+) \()?.*?([^\/\\]+\.js):(\d+):(\d+)/);
    if (!m) continue;
    const name = m[1] || '<top>';
    if (name === 'main0') break;
    out.push(name + '@' + m[3] + ':' + m[4]);
  }
  return out.join(' < ');
}

class Inner {
  constructor() { this.n = 0; }
  leaf(i) { if (i === 7) throw new Error('leaf ' + i); this.n += i; return this.n; }
  make(i) { return new Error('made ' + i); }
}
class Middle {
  constructor() { this.inner = new Inner(); }
  mid(i) { const r = this.inner.leaf(i); return r + 1; }
  midMake(i) { return this.inner.make(i); }
}
class Outer {
  constructor() { this.middle = new Middle(); }
  top(i) { return this.middle.mid(i) * 2; }
  topMake(i) { return this.middle.midMake(i); }
}

function oneLevel(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    try { s += o.middle.inner.leaf(i); } catch (e) { console.log('1:', e.message, frames(e.stack)); }
  }
  return s;
}

function twoLevels(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    try { s += o.middle.mid(i); } catch (e) { console.log('2:', e.message, frames(e.stack)); }
  }
  return s;
}

function threeLevels(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    try { s += o.top(i); } catch (e) { console.log('3:', e.message, frames(e.stack)); }
  }
  return s;
}

function notThrown(o, n) {
  let last = '';
  for (let i = 0; i < n; i++) last = frames(o.topMake(i).stack);
  return last;
}

function inFinally(o, n) {
  let log = [];
  for (let i = 0; i < n; i++) {
    try {
      try { o.top(i); } finally { if (i >= 6) log.push('finally ' + i); }
    } catch (e) { log.push('caught ' + e.message); }
  }
  return log.join(', ');
}

function main0() {
  console.log(oneLevel(new Outer(), 10));
  console.log(twoLevels(new Outer(), 10));
  console.log(threeLevels(new Outer(), 10));
  console.log(notThrown(new Outer(), 5));
  console.log(inFinally(new Outer(), 9));
  // The same sites warmed with many calls, then thrown through.
  const o = new Outer();
  let t = 0;
  for (let k = 0; k < 2000; k++) t += o.top(k % 7);
  console.log(t);
  console.log(threeLevels(o, 8));
}
main0();
