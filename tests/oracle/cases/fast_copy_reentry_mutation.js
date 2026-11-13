// What a guarded region's fast copy read once it reads once (a shape-guarded
// load repeated is the first, lower/guarded_load_fold.h), so everything that
// can store between two reads has to end that: a miss that leaves for the
// slow copy, whose accesses replace the objects the fast copy had read; a call
// in the loop; a store through another name for the same object.

class Point {
  constructor(observer) {
    this._x = 0;
    this._y = 0;
    this._observer = observer;
  }
  get x() { return this._x; }
  set x(v) {
    if (this._x !== v) {
      this._x = v;
      this._observer._onUpdate(this);
    }
  }
  get y() { return this._y; }
  set y(v) { this._y = v; }
}

let swaps = 0;

class Node {
  constructor(i) {
    this.id = i;
    this._position = new Point(this);
    this.v = i % 3 + 0.5;
    this.tick = 0;
    this.peer = null;
  }
  get x() { return this._position.x; }
  set x(v) { this._position.x = v; }
  get y() { return this._position.y; }
  set y(v) { this._position.y = v; }
  _onUpdate() { this.tick++; }
}

// A node whose update replaces its own point, and its peer's, in the middle
// of the iteration that stored into the old one.
class Swapper extends Node {
  _onUpdate(point) {
    this.tick++;
    swaps++;
    const fresh = new Point(this);
    fresh._x = point._x + 100;
    fresh._y = point._y - 100;
    this._position = fresh;
    if (this.peer) {
      const other = new Point(this.peer);
      other._x = -this.peer._position._x;
      other._y = swaps;
      this.peer._position = other;
    }
  }
}

function step(list) {
  let s = 0;
  for (let i = 0; i < list.length; i++) {
    const b = list[i];
    const before = b.x + b.y;
    b.x += b.v;
    // After the store's update ran: the point may be another object.
    const after = b.x + b.y;
    b.y = after - before;
    s += b.x * 3 + b.y + (b.peer ? b.peer.x + b.peer.y : 0);
  }
  return s;
}

function stepCalls(list, f) {
  let s = 0;
  for (let i = 0; i < list.length; i++) {
    const b = list[i];
    const x0 = b.x;
    f(b, i);
    s += x0 + b.x * 2 + b.y;
  }
  return s;
}

function stepAliases(list, others) {
  let s = 0;
  for (let i = 0; i < list.length; i++) {
    const a = list[i], b = others[i];
    const x0 = a.x;
    b.x = x0 + 1.5;
    const x1 = a.x;
    b.y = a.y + 0.25;
    s += x0 + x1 * 2 + a.y * 4 + b.x;
  }
  return s;
}

function make(n, swapEvery) {
  const list = [];
  for (let i = 0; i < n; i++) {
    const node = swapEvery > 0 && i % swapEvery === 0 ? new Swapper(i) : new Node(i);
    if (i > 0 && i % 4 === 0) node.peer = list[i - 1];
    list.push(node);
  }
  return list;
}

const plain = make(48, 0);
let total = 0;
for (let r = 0; r < 50; r++) total += step(plain);
console.log('plain', total.toFixed(2));

for (const every of [1, 2, 5, 12]) {
  const list = make(48, every);
  let t = 0;
  for (let r = 0; r < 30; r++) t += step(list);
  let ticks = 0;
  for (const b of list) ticks += b.tick;
  console.log('swap', every, t.toFixed(2), ticks, swaps);
}

// A call between the reads that replaces the point, stores into it, or does
// neither.
const callers = [
  (b) => {},
  (b) => { b._position._x += 1; },
  (b, i) => { if (i % 3 === 0) { const p = new Point(b); p._x = i; p._y = -i; b._position = p; } },
  (b, i) => { if (i % 5 === 1) Object.defineProperty(b, '_position', { value: b._position, writable: true, enumerable: false, configurable: true }); },
];
for (let c = 0; c < callers.length; c++) {
  const list = make(40, 0);
  let t = 0;
  for (let r = 0; r < 25; r++) t += stepCalls(list, callers[c]);
  console.log('call', c, t.toFixed(2));
}

// Two lists that share nodes, share points, or share nothing.
const left = make(32, 0);
const same = left;
const sharedPoints = make(32, 0);
for (let i = 0; i < 32; i++) if (i % 2 === 0) sharedPoints[i]._position = left[i]._position;
const apart = make(32, 0);
for (const [name, others] of [['same', same], ['points', sharedPoints], ['apart', apart]]) {
  let t = 0;
  for (let r = 0; r < 25; r++) t += stepAliases(left, others);
  console.log('alias', name, t.toFixed(2));
}
