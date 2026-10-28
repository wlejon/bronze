// A method compiled in place at its call sites (lower/inline_guarded.cpp) is
// only ever the body the call would have run: patching the prototype,
// deleting the method, shadowing it with an own property, swapping the
// receiver's prototype, a subclass override or an accessor in its place, each
// after the sites are warm, must reach whatever the property holds now.

class P {
  constructor() { this.x = 1; }
  get2() { return this.x * 2; }
  add(v) { this.x += v; return this; }
}
class Q extends P {
  get2() { return -this.x; }
}
class Box {
  constructor() { this.p = new P(); }
}

function sum(b, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += b.p.get2();
  return s;
}
function bump(b, n) {
  for (let i = 0; i < n; i++) b.p.add(1);
  return b.p.x;
}

const b = new Box();
console.log('warm', sum(b, 1000), bump(b, 1000));

// Patched on the prototype.
const original = P.prototype.get2;
P.prototype.get2 = function () { return 100; };
console.log('patched', sum(b, 3));
P.prototype.get2 = original;
console.log('restored', sum(b, 3));

// An own property shadows the prototype's method.
b.p.get2 = () => 7;
console.log('shadowed', sum(b, 3));
delete b.p.get2;
console.log('unshadowed', sum(b, 3));

// Swapped prototype.
Object.setPrototypeOf(b.p, Q.prototype);
console.log('swapped', sum(b, 3));
Object.setPrototypeOf(b.p, P.prototype);
console.log('swapped back', sum(b, 3));

// A subclass instance at the same site.
const c = new Box();
c.p = new Q();
console.log('subclass', sum(c, 3), sum(b, 3));

// An accessor where the method was.
let reads = 0;
Object.defineProperty(P.prototype, 'get2', { configurable: true, get() { reads++; return () => 5; } });
console.log('accessor', sum(b, 4), reads);
Object.defineProperty(P.prototype, 'get2', { configurable: true, writable: true, value: original });
console.log('value again', sum(b, 2));

// Deleted: the call throws, and a caller in the loop catches it.
delete P.prototype.get2;
try { sum(b, 2); } catch (e) { console.log('deleted', e instanceof TypeError); }
P.prototype.get2 = original;

// A callee replaced from inside a call it made: the next iteration sees it.
let flips = 0;
P.prototype.add = function (v) {
  this.x += v;
  if (++flips === 3) P.prototype.add = function (w) { this.x -= w; return this; };
  return this;
};
console.log('self-replaced', bump(b, 6));

// A getter on the receiver used as the method's own data.
class G {
  constructor() { this._v = 3; }
  get v() { return this._v; }
  twice() { return this.v * 2; }
}
class HoldG { constructor() { this.g = new G(); } }
function twices(h, n) { let s = 0; for (let i = 0; i < n; i++) s += h.g.twice(); return s; }
const hg = new HoldG();
console.log('getter', twices(hg, 100));
Object.defineProperty(G.prototype, 'v', { get() { return 10; } });
console.log('getter patched', twices(hg, 3));
