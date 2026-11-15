// Temporaries a loop makes and drops: every result must match node, in a plain
// build and in one built on its own profile.
const out = [];
class V3 { constructor(x, y, z) { this.x = x; this.y = y; this.z = z; }
  add(v) { return new V3(this.x + v.x, this.y + v.y, this.z + v.z); }
  scale(s) { return new V3(this.x * s, this.y * s, this.z * s); }
  dot(v) { return this.x * v.x + this.y * v.y + this.z * v.z; }
  len2() { return this.dot(this); } }
const a = new V3(1, 2, 3);

// 1. the plain chain
function chain(n) { let s = 0; for (let i = 0; i < n; i++) { const c = new V3(i, 1, 2).add(a).scale(0.5); s += c.dot(a); } return s; }
out.push(chain(20000));

// 2. one instance in many escapes
let kept = null;
function escapes(n) { let s = 0; for (let i = 0; i < n; i++) { const c = new V3(i, 1, 2).add(a); if (i % 7919 === 0) kept = c; s += c.x; } return s; }
out.push(escapes(30000), kept.x, kept.y, kept instanceof V3, Object.keys(kept).join());

// 3. a non-number argument arrives late: string concatenation through `add`
function mixed(n) { let s = ''; let t = 0; for (let i = 0; i < n; i++) { const c = new V3(i === n - 3 ? 'q' : i, 1, 2).add(a); if (i > n - 6) s += c.x + '|'; else t += c.x; } return s + t; }
out.push(mixed(20000));

// 4. a method replaced while the loop runs
class P { constructor(v) { this.v = v; } twice() { return new P(this.v * 2); } get() { return this.v; } }
function swap(n) { let s = 0; for (let i = 0; i < n; i++) { if (i === n - 100) P.prototype.twice = function () { return new P(this.v * 3); }; s += new P(i).twice().get(); } return s; }
out.push(swap(20000));

// 5. a setter appears on the prototype for a key the constructor writes
class Q { constructor(v) { this.v = v; this.w = v + 1; } sum() { return this.v + this.w; } }
let setterHits = 0;
function setter(n) { let s = 0; for (let i = 0; i < n; i++) { if (i === n - 50) Object.defineProperty(Q.prototype, 'w', { set(x) { setterHits++; }, get() { return 1000; }, configurable: true }); s += new Q(i).sum(); } return s; }
out.push(setter(20000), setterHits);

// 6. two closures of one class body in turn
function mk() { return class K { constructor(v) { this.v = v; } inc() { return new K(this.v + 1); } val() { return this.v; } }; }
const K1 = mk(), K2 = mk();
function two(n) { let s = 0; for (let i = 0; i < n; i++) { const C = i & 1 ? K1 : K2; s += new C(i).inc().val(); } return s; }
out.push(two(20000));

// 7. a getter on the other operand runs code that looks at prototypes
let peeks = 0;
const spy = { get x() { peeks++; return 5; }, y: 6, z: 7 };
function getter(n) { let s = 0; for (let i = 0; i < n; i++) { const o = i === n - 10 ? spy : a; s += new V3(i, 1, 2).add(o).dot(a); } return s; }
out.push(getter(20000), peeks);

// 8. a throw in the middle of the chain
class T { constructor(v) { this.v = v; } half() { if (this.v === 19990) throw new Error('at ' + this.v); return new T(this.v / 2); } }
function thrower(n) { let s = 0; try { for (let i = 0; i < n; i++) s += new T(i).half().v; } catch (e) { return s + ':' + e.message; } return s; }
out.push(thrower(20000));

// 9. object and string fields, nested temporaries
class Seg { constructor(p, q, name) { this.p = p; this.q = q; this.name = name; }
  mid() { return new V3((this.p.x + this.q.x) / 2, (this.p.y + this.q.y) / 2, (this.p.z + this.q.z) / 2); }
  tag() { return this.name + ':' + this.p.x; } }
function nested(n) { let s = 0; let t = ''; for (let i = 0; i < n; i++) { const g = new Seg(new V3(i, 0, 0), new V3(0, i, 0), 'g' + (i & 3)); s += g.mid().len2(); if (i > n - 4) t += g.tag() + ' '; } return s + ' ' + t; }
out.push(nested(20000));

// 10. identity and reflection on an instance that mostly stays unseen
function reflect(n) { let s = 0; for (let i = 0; i < n; i++) { const c = new V3(i, i + 1, i + 2); if (i % 5000 === 0) { s += Object.keys(c).length + (Object.getPrototypeOf(c) === V3.prototype ? 10 : 0) + (c === c ? 1 : 0); } s += c.z; } return s; }
out.push(reflect(20000));

// 11. NaN, -0 and big values through the fields
function odd(n) { let s = 0; let z = 0; for (let i = 0; i < n; i++) { const c = new V3(i === 7 ? NaN : i, -0, 1e308).scale(i === 9 ? 1e10 : 1); if (c.x !== c.x) z++; if (Object.is(c.y, -0)) z += 2; if (c.z === Infinity) z += 100000; s += c.y; } return z + ':' + Object.is(s, -0); }
out.push(odd(20000));

// 12. the prototype swapped for a plain function constructor
function F(v) { this.v = v; }
F.prototype.get = function () { return this.v; };
function protoSwap(n) { let s = 0; for (let i = 0; i < n; i++) { if (i === n - 20) F.prototype = { get() { return -1; } }; s += new F(i).get(); } return s; }
out.push(protoSwap(20000));

console.log(out.join('\n'));
