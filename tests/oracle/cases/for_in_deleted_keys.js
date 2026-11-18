// `for-in` and keys removed while the loop runs (14.7.5.9
// EnumerateObjectProperties: "A property that is deleted before it is
// processed is not processed"). bronze snapshots the keys when the loop
// starts and, while nothing anywhere has removed a property since, visits
// them as they are; once something has (a delete, a prototype swap, an array
// element or length change, a resizable buffer's shrink, any proxy), each
// remaining key is asked for again first, the way V8's ForInFilter asks:
// present anywhere on the chain, or, for a proxy level, its
// getOwnPropertyDescriptor trap's answer. Keys added during the loop are not
// visited. Plain and dictionary-mode objects, integer-like keys, arrays (holes,
// pop, length truncation, splice, accessor elements), typed arrays over a
// resizable buffer, strings and String wrappers, arguments objects,
// functions, prototype keys deleted, shadowed and unshadowed, a prototype
// swap, Proxy receivers and proxies on the chain (trap order logged), nested
// loops over one shape, a generator suspended mid-loop, and a delete and
// re-add of a key not yet visited.
const out = [];
const walk = (o, at) => {
  const seen = [];
  for (const k in o) { seen.push(k); if (at) at(k, o); }
  return seen.join(',');
};

// Plain object: ahead, current, behind; and an add.
{
  const o = { a: 1, b: 2, c: 3, d: 4, e: 5 };
  out.push(walk(o, (k) => { if (k === 'b') { delete o.d; delete o.b; delete o.a; o.f = 6; } }));
  out.push(Object.keys(o).join(','));
}
// Delete then re-add a key not yet visited.
{
  const o = { a: 1, b: 2, c: 3 };
  out.push(walk(o, (k) => { if (k === 'a') { delete o.c; o.c = 9; } }));
  const p = { a: 1, b: 2, c: 3 };
  out.push(walk(p, (k) => { if (k === 'a') { delete p.b; p.b = 9; } }));
}
// A delete somewhere else entirely does not drop a live key.
{
  const o = { a: 1, b: 2, c: 3 };
  const other = { z: 1 };
  out.push(walk(o, (k) => { if (k === 'a') delete other.z; }));
}
// Dictionary-mode object, integer-like keys.
{
  const d = {};
  for (let i = 0; i < 40; i++) d['k' + i] = i;
  delete d.k0;
  d[7] = 'x'; d[3] = 'y'; d[100] = 'z';
  let n = 0;
  const seen = [];
  for (const k in d) {
    seen.push(k);
    if (k === '3') { delete d[100]; delete d.k5; delete d.k39; }
    if (k === 'k10') { for (let i = 11; i < 30; i += 2) delete d['k' + i]; }
    n++;
  }
  out.push(n, seen.join(','));
}
// Arrays: a hole made ahead, pop, length truncation, splice, an accessor
// element, a named property.
{
  const a = [10, 11, 12, 13, 14, 15];
  out.push(walk(a, (k) => { if (k === '1') { delete a[3]; a.pop(); } }));
  const b = [0, 1, 2, 3, 4, 5, 6];
  out.push(walk(b, (k) => { if (k === '2') b.length = 4; }));
  const c = [0, 1, 2, 3, 4, 5];
  c.extra = 'e';
  out.push(walk(c, (k) => { if (k === '0') { c.splice(1, 2); delete c.extra; } }));
  const h = [0, , 2, , 4];
  out.push(walk(h, (k) => { if (k === '0') { h[1] = 1; delete h[4]; } }));
  const x = [0, 1, 2, 3];
  Object.defineProperty(x, 2, { get() { return 'g'; }, enumerable: true, configurable: true });
  Object.defineProperty(x, 3, { value: 3, enumerable: false, configurable: true });
  out.push(walk(x, (k) => { if (k === '0') delete x[2]; }));
  const y = [0, 1, 2, 3];
  Object.defineProperty(y, 1, { get() { return 'g'; }, enumerable: true, configurable: false });
  out.push(walk(y, (k) => { if (k === '0') { delete y[1]; y.length = 1; } }), y.length);
  const s = [1, 2, 3];
  out.push(walk(s, (k) => { if (k === '0') s.shift(); }));
}
// Typed arrays over a resizable buffer.
{
  const buf = new ArrayBuffer(8, { maxByteLength: 16 });
  const t = new Uint8Array(buf);
  out.push(walk(t, (k) => { if (k === '2') buf.resize(5); }));
  const u = new Int16Array(4);
  u.tag = 1;
  out.push(walk(u, (k) => { if (k === '0') { delete u.tag; delete u[1]; } }));
}
// Strings and String wrappers, while something else is deleted.
{
  const junk = { q: 1, r: 2 };
  out.push(walk('abc', (k) => { if (k === '0') delete junk.q; }));
  const w = new String('xy');
  w.extra = 1;
  w.more = 2;
  out.push(walk(w, (k) => { if (k === '0') { delete w.more; delete w[1]; } }));
}
// Arguments objects.
{
  function f() { return walk(arguments, (k) => { if (k === '0') { delete arguments[2]; delete arguments[1]; arguments[1] = 'b'; } }); }
  out.push(f('a', 'b', 'c', 'd'));
  function g(p, q, r) { return walk(arguments, (k) => { if (k === '0') delete arguments[1]; }); }
  out.push(g(1, 2, 3));
  function s(p, q) { 'use strict'; return walk(arguments, (k) => { if (k === '1') delete arguments[0]; }); }
  out.push(s(1, 2));
}
// Functions' own keys.
{
  function fn() {}
  fn.a = 1; fn.b = 2; fn.c = 3;
  out.push(walk(fn, (k) => { if (k === 'a') delete fn.c; }));
  class K { static s1 = 1; static s2 = 2; }
  class L extends K { static t = 3; }
  out.push(walk(L, (k) => { if (k === 't') delete K.s2; }));
}
// The prototype chain.
{
  const proto = { p1: 1, p2: 2, p3: 3, shared: 'proto' };
  const o = Object.create(proto);
  o.own = 1; o.shared = 'own';
  out.push(walk(o, (k) => { if (k === 'own') { delete proto.p2; } }));
  // An own key deleted that shadowed an inherited one: still there, by the chain.
  const o2 = Object.create(proto);
  o2.a = 1; o2.p3 = 'own';
  out.push(walk(o2, (k) => { if (k === 'a') delete o2.p3; }));
  // Shadowed by a non-enumerable own property mid-loop: still present.
  const o3 = Object.create(proto);
  o3.a = 1;
  out.push(walk(o3, (k) => { if (k === 'a') Object.defineProperty(o3, 'p3', { value: 0, enumerable: false }); }));
  // Made non-enumerable mid-loop: still present.
  const o4 = { a: 1, b: 2, c: 3 };
  out.push(walk(o4, (k) => { if (k === 'a') Object.defineProperty(o4, 'c', { enumerable: false }); }));
  // The receiver's prototype swapped mid-loop.
  const o5 = Object.create({ i1: 1, i2: 2 });
  o5.a = 1;
  out.push(walk(o5, (k) => { if (k === 'a') Object.setPrototypeOf(o5, { i2: 'new', i9: 9 }); }));
  // A prototype's prototype swapped.
  const mid = Object.create({ top1: 1, top2: 2 });
  mid.m = 1;
  const o6 = Object.create(mid);
  o6.a = 1;
  out.push(walk(o6, (k) => { if (k === 'm') Object.setPrototypeOf(mid, null); }));
  // A key on Object.prototype, deleted mid-loop.
  Object.prototype.zz = 1;
  out.push(walk({ a: 1, b: 2 }, (k) => { if (k === 'a') delete Object.prototype.zz; }));
  Object.prototype.zz = 1;
  out.push(walk([5, 6], (k) => { if (k === '0') delete Object.prototype.zz; }));
  delete Object.prototype.zz;
}
// Proxies: a receiver, and a link of the chain; the trap order is logged.
{
  const log = [];
  const handler = {};
  for (const t of ['ownKeys', 'getPrototypeOf', 'getOwnPropertyDescriptor', 'has', 'get', 'deleteProperty']) {
    handler[t] = (...args) => {
      log.push(t + (typeof args[1] === 'string' ? ':' + args[1] : ''));
      return Reflect[t](...args);
    };
  }
  const target = { a: 1, b: 2, c: 3 };
  Object.defineProperty(target, 'hidden', { value: 1, enumerable: true, configurable: true });
  const p = new Proxy(target, handler);
  out.push(walk(p, (k) => { log.push('body:' + k); if (k === 'a') { delete p.c; Object.defineProperty(target, 'hidden', { enumerable: false }); } }));
  out.push(log.join(' '));
  log.length = 0;
  const pt = { x: 1, y: 2 };
  const child = Object.create(new Proxy(pt, handler));
  child.own = 1;
  out.push(walk(child, (k) => { log.push('body:' + k); if (k === 'own') delete pt.y; }));
  out.push(log.join(' '));
  log.length = 0;
  const lying = new Proxy({}, {
    ownKeys() { log.push('ownKeys'); return ['m', 'n', 'o']; },
    getOwnPropertyDescriptor(t, k) {
      log.push('gopd:' + k);
      if (k === 'n') return undefined;
      return { value: 1, enumerable: k !== 'o', configurable: true };
    },
  });
  out.push(walk(lying, (k) => log.push('body:' + k)));
  out.push(log.join(' '));
}
// Nested loops over objects of one shape (one cached key list).
{
  const mk = () => ({ a: 1, b: 2, c: 3 });
  const o1 = mk(), o2 = mk();
  const seen = [];
  for (const k1 in o1) {
    for (const k2 in o2) {
      seen.push(k1 + k2);
      if (k1 === 'a' && k2 === 'a') { delete o2.b; delete o1.c; }
    }
  }
  out.push(seen.join(','));
}
// A generator suspended inside the loop, the key deleted while it waits.
{
  const o = { a: 1, b: 2, c: 3, d: 4 };
  function* keys(obj) { for (const k in obj) yield k; }
  const it = keys(o);
  const seen = [it.next().value];
  delete o.c;
  for (let r = it.next(); !r.done; r = it.next()) seen.push(r.value);
  out.push(seen.join(','));
}
// break and continue with deletions, and a labelled continue out of an inner loop.
{
  const o = { a: 1, b: 2, c: 3, d: 4, e: 5 };
  const seen = [];
  outer: for (const k in o) {
    if (k === 'a') { delete o.b; continue; }
    for (const j of [1]) { if (k === 'c') { delete o.d; continue outer; } }
    seen.push(k);
    if (k === 'e') break;
  }
  out.push(seen.join(','));
}
// Many keys, every other one deleted ahead from the middle of the walk.
{
  const o = {};
  for (let i = 0; i < 64; i++) o['p' + i] = i;
  let sum = 0;
  for (const k in o) {
    sum += o[k];
    if (k === 'p20') for (let i = 21; i < 64; i += 2) delete o['p' + i];
  }
  out.push(sum);
}
console.log(out.join('\n'));
