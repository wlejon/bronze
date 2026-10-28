// Direct accessor and method edges handed receivers their guess is wrong for.
//
// A `get x()` / `set x(v)` a class declares is called directly at a site whose
// receiver inference typed as that class, behind a cell naming the shape and
// prototype epoch it was found for (il_accessor_direct.cpp,
// runtime/accessor_cell.cpp). A call such as `this._onChange()`, whose key
// some receiver holds as an own closure, reads that closure out of the slot
// the method cache names (il_lowering_direct.cpp lower_method_slot_inline).
// Each case warms the sites, then changes what the key means: an own property
// in front of the accessor, an own callback in front of the method, a
// subclass's override, another prototype, the prototype's accessor redefined
// or deleted. Every line must read as the ordinary [[Get]] / [[Set]] / Call.

const N = 2000;
let hits = 0;

class Euler {
  constructor() {
    this._x = 0;
    this._y = 0;
  }
  get x() { return this._x; }
  set x(v) { this._x = v; this._onChange(); }
  get y() { return this._y; }
  set y(v) { this._y = v; this._onChange(); }
  _onChange() {}
  // `this` is an Euler to inference, so these sites guess Euler's accessors
  // whatever reaches them: a subclass's instance, or a borrowed receiver.
  addX(d) {
    this.x += d;
    return this.x;
  }
}
class Doubled extends Euler {
  get x() { return this._x * 2; }
  set x(v) { this._x = v + 1; }
}
class Same extends Euler {}

function bump(e, d) { return e.addX(d); }
function readY(e) { return e.y; }
function setY(e, v) { e.y = v; }

function warm(e, d) {
  let r = 0;
  for (let i = 0; i < N; i++) r = bump(e, d);
  return r;
}

const e = new Euler();
console.log('warm', warm(e, 1), e._x);

// 1. The callback as an own closure, alone and alternating with receivers that
//    reach the prototype's no-op through the same site; then as an own
//    function using `this`, deleted, and replaced by a non-function.
const e2 = new Euler();
let calls = 0;
e2._onChange = () => { calls++; };
console.log('own callback', warm(e2, 2), calls);
for (let i = 0; i < 10; i++) bump(i % 2 ? e : e2, 1);
console.log('mixed callback', e.x, e2.x, calls);
const e3 = new Euler();
e3._onChange = function () { this.seen = (this.seen || 0) + 1; };
console.log('own function', warm(e3, 1), e3.seen);
delete e3._onChange;
console.log('own deleted', bump(e3, 1), e3.seen);
e3._onChange = 5;
let threw = '';
try { bump(e3, 1); } catch (err) { threw = err.constructor.name; }
console.log('own non-function', threw, e3._x);

// 2. An own data property in front of the accessor, then deleted.
const e4 = new Euler();
Object.defineProperty(e4, 'x', { value: 10, writable: true, configurable: true });
console.log('own x', warm(e4, 5), e4._x);
delete e4.x;
console.log('own x deleted', bump(e4, 1), e4._x);

// 3. A subclass's override, and a subclass that inherits.
const d = new Doubled();
console.log('subclass', bump(d, 3), bump(d, 3), d._x);
const s = new Same();
console.log('subclass inherits', warm(s, 7), s._x);

// 4. Another prototype under a warmed receiver, between it and Euler's with an
//    `x` of its own, and back.
const o = new Euler();
warm(o, 1);
Object.setPrototypeOf(o, {
  __proto__: Euler.prototype,
  get x() { return 100; },
  set x(v) { this._x = -v; },
});
console.log('setPrototypeOf', bump(o, 1), o._x);
Object.setPrototypeOf(o, Euler.prototype);
console.log('setPrototypeOf back', bump(o, 1), o._x);

// 5. Borrowed receivers: a number, whose `x` reads as undefined and whose
//    write throws in the class's strict code, and null, which throws first.
const e5 = new Euler();
for (let i = 0; i < N; i++) setY(e5, i);
threw = '';
try { Euler.prototype.addX.call(5, 1); } catch (err) { threw = err.constructor.name; }
console.log('y', readY(e5), threw);
threw = '';
try { Euler.prototype.addX.call(null, 1); } catch (err) { threw = err.constructor.name; }
console.log('null', threw);

// 6. The prototype's method replaced: the receivers without a callback of
//    their own reach the new one, the one with a callback keeps it.
Euler.prototype._onChange = function () { hits++; };
setY(e5, 1);
bump(e, 1);
bump(e2, 1);
console.log('proto method replaced', hits, e.x, calls);

// 7. The prototype's accessor redefined, seen through the class and a
//    subclass that inherits it, not through the one that overrides it.
Object.defineProperty(Euler.prototype, 'x', {
  get() { return this._x + 1000; },
  set(v) { this._x = v * 10; },
  configurable: true,
});
console.log('redefined', bump(e, 1), e._x);
console.log('redefined inherited', bump(s, 1), s._x);
console.log('subclass own', bump(d, 3), d._x);

// 8. The prototype's accessor deleted: the read finds nothing, the write adds
//    an own data property.
delete Euler.prototype.y;
console.log('deleted', String(readY(e5)));
setY(e5, 3);
console.log('deleted set', readY(e5), e5._y, Object.keys(e5).join(','));
