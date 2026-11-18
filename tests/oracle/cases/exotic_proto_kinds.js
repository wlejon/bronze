// Other exotic objects as prototypes (runtime/rt_exotic_link.cpp): a typed
// array (its numeric keys are its elements: a valid index is inherited data,
// a write to the heir lands on the heir, a write at an invalid index is a
// successful no-op, 10.4.5.5), a String wrapper (its indices and `length`
// are non-writable data), an `arguments` object, an Array subclass's
// instance, a function (its statics, `length`, `name`), each read, asked
// with `in`, enumerated and written through.
const out = [];
const show = (...xs) =>
  out.push(xs.map((x) => (x === undefined ? 'u' : typeof x === 'string' ? x : JSON.stringify(x))).join(' '));
const forIn = (o) => { const ks = []; for (const k in o) ks.push(k); return ks; };

const ta = new Uint8Array([3, 4]);
const t = Object.create(ta);
show('typed read', t[0], t[1], t[2], t['-0'], t[1.5]);
show('typed in', 0 in t, 1 in t, 2 in t, '-0' in t, 'byteLength' in t);
show('typed for-in', forIn(t));
ta[0] = 30;
show('typed after write', t[0]);
t[0] = 99;
show('typed heir write', t[0], ta[0], t.hasOwnProperty(0));
t[5] = 1;
show('typed invalid index', t[5], t.hasOwnProperty(5), 5 in t);
try { show('typed length', t.length); } catch (e) { show('typed length', e.constructor.name); }

const sw = new String('ab');
const s = Object.create(sw);
show('string read', s[0], s[1], s[2], s.length, s.toUpperCase === String.prototype.toUpperCase);
show('string in', 0 in s, 1 in s, 2 in s, 'length' in s);
show('string for-in', forIn(s));
s[0] = 'z';
show('string sloppy write', s[0], s.hasOwnProperty(0));
try {
  (function () { 'use strict'; s[1] = 'q'; })();
  show('string strict write', 'no throw');
} catch (e) {
  show('string strict write', e.constructor.name);
}
s[2] = 'c';
show('string past end', s[2], s.hasOwnProperty(2));

const args = (function () { return arguments; })(4, 5);
const g = Object.create(args);
show('arguments', g[0], g[1], g.length, 1 in g, forIn(g));

class X extends Array {}
const x = new X();
x.push(3, 6);
const xh = Object.create(x);
show('subclass', xh[0], xh[1], xh.length, xh instanceof X, Array.isArray(xh), forIn(xh));
x.pop();
show('subclass after pop', xh[1], xh.length);

function f(a, b) {}
f[0] = 'f0';
f.extra = 'ex';
const fh = Object.create(f);
show('function', fh[0], fh.extra, fh.length, fh.name, 0 in fh, 'name' in fh, typeof fh.call);
fh.extra = 'mine';
show('function heir write', fh.extra, f.extra);

console.log(out.join('\n'));
