// Accessor sites a type-feedback profile names (docs/type-feedback.md, "F,
// accessor targets"): reads and writes through receivers inference cannot
// type (array elements, parameters of functions reached from everywhere),
// whose profiled shape is a class instance that inherits a getter and setter
// from two classes up. A build reading the profile calls those halves
// directly, or inlines them, behind the site's accessor cell
// (runtime/accessor_cell.cpp). After the warm-up the same sites meet
// receivers the guess is wrong for: a subclass's override, the accessor
// redefined on the prototype, a plain object with an own data property, a
// receiver whose prototype was replaced, and non-objects. Every line must read
// as the ordinary [[Get]] / [[Set]].

class Point {
  constructor(x) {
    this._x = x;
    this._sets = 0;
  }
  get x() { return this._x; }
  set x(v) { this._x = v; this._sets++; }
}
class Middle extends Point {}
class Leaf extends Middle {
  constructor(x) {
    super(x);
    this._pos = new Point(x * 2);
  }
  // A getter reading another accessor site, as pixi's `Container.x` reads
  // `this._position.x`.
  get px() { return this._pos.x; }
  set px(v) { this._pos.x = v; }
}

function readX(o) { return o.x; }
function writeX(o, v) { o.x = v; }
function readPx(o) { return o.px; }
function writePx(o, v) { o.px = v; }

function sumX(list) {
  let s = 0;
  for (let i = 0; i < list.length; i++) s += readX(list[i]);
  return s;
}
function bumpX(list, d) {
  for (let i = 0; i < list.length; i++) writeX(list[i], readX(list[i]) + d);
}
function sumPx(list) {
  let s = 0;
  for (let i = 0; i < list.length; i++) {
    writePx(list[i], readPx(list[i]) + 1);
    s += list[i].px;
  }
  return s;
}

const leaves = [];
for (let i = 0; i < 64; i++) leaves.push(new Leaf(i));
let total = 0;
for (let round = 0; round < 200; round++) {
  bumpX(leaves, 1);
  total += sumX(leaves) + sumPx(leaves);
}
console.log('warm', total, leaves[3]._x, leaves[3]._sets, leaves[3]._pos._x, leaves[3]._pos._sets);

// (a) A subclass overriding the getter and setter.
class Odd extends Leaf {
  get x() { return -this._x; }
  set x(v) { this._x = v * 3; }
  get px() { return 7; }
}
const odd = new Odd(5);
bumpX([odd], 2);
console.log('override', readX(odd), odd._x, odd._sets, sumX([odd, leaves[0]]), readPx(odd));
writePx(odd, 9);
console.log('override px', odd._pos._x, readPx(odd));

// (b) A plain object with an own data property of the name, and one with an
//     own accessor.
const plain = { x: 41, px: 'p', _x: 0 };
bumpX([plain], 1);
writePx(plain, 'q');
console.log('own data', readX(plain), plain._x, readPx(plain), sumX([plain, leaves[1]]));
let seen = 0;
const own = { get x() { return 99; }, set x(v) { seen = v; } };
writeX(own, 4);
console.log('own accessor', readX(own), seen);

// (c) A receiver whose prototype was replaced, then restored.
const swapped = new Leaf(10);
Object.setPrototypeOf(swapped, {
  get x() { return 'swapped'; },
  set x(v) { this._x = 'set ' + v; },
});
writeX(swapped, 3);
console.log('swapped', readX(swapped), swapped._x, String(readPx(swapped)));
Object.setPrototypeOf(swapped, Leaf.prototype);
writeX(swapped, 4);
console.log('restored', readX(swapped), swapped._x, swapped._sets, readPx(swapped));

// (d) Non-objects: a number or a string reads undefined; undefined throws.
console.log('number', String(readX(5)), String(readPx('s')));
let threw = '';
try { readX(undefined); } catch (err) { threw = err.constructor.name; }
console.log('undefined', threw);

// (e) The accessor redefined on the prototype the sites resolved to: every
//     warmed receiver now reaches the new halves.
Object.defineProperty(Point.prototype, 'x', {
  get() { return this._x + 1000; },
  set(v) { this._x = v - 1; },
  configurable: true,
});
bumpX(leaves, 1);
console.log('redefined', sumX(leaves), leaves[3]._x, leaves[3]._sets, sumPx(leaves), leaves[3]._pos._x);
console.log('redefined override', readX(odd));

// (f) A data property put in front of it on the middle class's prototype:
//     a write adds an own property to the receiver.
Object.defineProperty(Middle.prototype, 'x', { value: 5, writable: true, configurable: true });
writeX(leaves[2], 77);
console.log('shadowed', sumX(leaves), readX(leaves[2]), leaves[2]._x, readPx(leaves[2]));
delete Middle.prototype.x;
console.log('unshadowed', readX(leaves[2]), readX(new Point(1)));
