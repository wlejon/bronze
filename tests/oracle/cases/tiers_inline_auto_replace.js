// Guarded method inlining in the tiered engine (lower/inline_guarded.h): the
// body of `o.val(i)` is compiled into each loop below behind the call site's
// method cache, and the loops run long enough to be OSR'd and compiled at
// tier 2 with it. Whatever changes the method the call would enter, after
// that code is installed or in the middle of its loop, sends the call back to
// the real dispatch: a prototype method replaced (the prototype epoch), an own
// property shadowing it and then deleted, the receiver's prototype changed.
// Generator and async callers run the same inlined call across their
// suspensions, and an exception thrown out of an inlined body in a resumed
// async function names that function's frame and the callee's.

function frames(stack) {
  const out = [];
  for (const line of stack.split('\n').slice(1)) {
    const m = line.match(/at (?:(\S+) \()?.*?([^\/\\]+\.js):(\d+):(\d+)/);
    if (!m) continue;
    const name = m[1] || '<top>';
    if (name === 'main') break;
    out.push(name + '@' + m[3] + ':' + m[4]);
  }
  return out.join(' < ');
}

class Base {
  val(i) { return i + 1; }
}
class Sub extends Base {
  val(i) { return i * 2; }
}
const original = Base.prototype.val;

function run(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += o.val(i);
  return s;
}

function runSwitching(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    if (i === n / 2) Base.prototype.val = function (k) { return k + 5; };
    s += o.val(i);
  }
  return s;
}

function* gen(o, n) {
  for (let i = 0; i < n; i++) yield o.val(i);
}

async function arun(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    s += o.val(i);
    if (i % 25000 === 0) await null;
  }
  return s;
}

class Thrower {
  boom(i) {
    if (i === 99999) {
throw new Error('boom');
    }
    return 1;
  }
}

async function athrow(t, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    s +=
t.boom(i);
    if (i % 25000 === 0) await null;
  }
  return s;
}

async function main() {
  const n = 200000;
  const b = new Base();
  console.log('proto', run(b, n));
  Base.prototype.val = function (i) { return i + 2; };
  console.log('replaced', run(b, n));
  b.val = function (i) { return i + 3; };
  console.log('own', run(b, n));
  delete b.val;
  console.log('deleted', run(b, n));
  Object.setPrototypeOf(b, Sub.prototype);
  console.log('setPrototypeOf', run(b, n));

  Base.prototype.val = original;
  console.log('switching', runSwitching(new Base(), n));

  Base.prototype.val = original;
  let g = 0;
  for (const v of gen(new Base(), 100000)) g += v;
  console.log('generator', g);
  console.log('async', await arun(new Base(), 100000));
  try {
    await athrow(new Thrower(), 100000);
  } catch (e) {
    console.log('async throw', e.message, frames(e.stack));
  }
}
main();
