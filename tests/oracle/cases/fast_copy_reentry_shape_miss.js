// A loop over objects with inlined accessors is copied (lower/guard_region.h):
// in the fast copy a shape test that fails leaves for the slow copy for the
// rest of that iteration, and the back edge tries the fast copy again
// ("RE-ENTRY"). The lists here mix the common layout with objects that miss
// in the middle of an iteration: other key orders, an own accessor, an accessor
// in a point, a subclass, velocities that are not Numbers.

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
  set y(v) {
    if (this._y !== v) {
      this._y = v;
      this._observer._onUpdate(this);
    }
  }
}

class Node {
  constructor(i) {
    this.id = i;
    this._position = new Point(this);
    this.vx = (i % 7) - 3.5;
    this.vy = (i % 5) - 2.25;
    this.tick = 0;
    this.dirty = false;
  }
  get x() { return this._position.x; }
  set x(v) { this._position.x = v; }
  get y() { return this._position.y; }
  set y(v) { this._position.y = v; }
  _onUpdate(point) {
    this.tick++;
    if (this.dirty) return;
    this.dirty = true;
  }
}

class Wide extends Node {
  constructor(i) {
    super(i);
    this.extra = i;
  }
}

function step(list, w, h) {
  for (let i = 0; i < list.length; i++) {
    const b = list[i];
    b.x += b.vx;
    b.y += b.vy;
    b.vy += 0.5;
    if (b.x > w) { b.vx *= -1; b.x = w; }
    else if (b.x < 0) { b.vx *= -1; b.x = 0; }
    if (b.y > h) { b.vy *= -0.85; b.y = h; }
    else if (b.y < 0) { b.vy = 0; b.y = 0; }
  }
}

function digest(list) {
  let s = 0, t = 0;
  for (let i = 0; i < list.length; i++) {
    const b = list[i];
    const x = b.x, y = b.y;
    s += (typeof x === 'number' ? x : 1e3) * (i + 1) + (typeof y === 'number' ? y : 1e3);
    t += b.tick + (b.dirty ? 1 : 0);
    b.dirty = false;
  }
  return s.toFixed(3) + ' ' + t;
}

function odd(i, kind) {
  switch (kind) {
    case 0: {
      // The same keys in another order: every static slot is elsewhere.
      const o = Object.create(Node.prototype);
      o.dirty = false; o.tick = 0; o.vy = 1; o.vx = 2;
      o._position = new Point(o); o.id = i;
      return o;
    }
    case 1: {
      // An own accessor over the field the getters read.
      const o = new Node(i);
      const p = o._position;
      let reads = 0;
      Object.defineProperty(o, '_position', { get() { reads++; o.tick += reads % 3 === 0 ? 1 : 0; return p; }, configurable: true });
      return o;
    }
    case 2: {
      // A point whose `_x` is an own accessor pair that counts its stores.
      const o = new Node(i);
      let held = 0, stores = 0;
      Object.defineProperty(o._position, '_x', {
        get() { return held; },
        set(v) { stores++; held = stores % 5 === 0 ? v + 1 : v; },
        enumerable: true, configurable: true,
      });
      return o;
    }
    case 3:
      return new Wide(i);
    case 4: {
      // A point of another layout behind the same accessors.
      const o = new Node(i);
      const q = Object.create(Point.prototype);
      q._observer = o; q._y = 3; q._x = 4;
      o._position = q;
      return o;
    }
    default: {
      // Velocities that are not Numbers: the number tests leave too.
      const o = new Node(i);
      o.vx = '1';
      o.vy = { valueOf() { return 0.25; } };
      return o;
    }
  }
}

function build(n, every) {
  const list = [];
  for (let i = 0; i < n; i++) list.push(every > 0 && i % every === every - 1 ? odd(i, (i / every | 0) % 6) : new Node(i));
  return list;
}

// Warm the common layout first, so the sites hold it.
const plain = build(64, 0);
for (let r = 0; r < 60; r++) step(plain, 40, 30);
console.log('plain', digest(plain));

for (const every of [1, 2, 3, 7, 16, 63]) {
  const list = build(96, every);
  for (let r = 0; r < 40; r++) step(list, 40, 30);
  console.log('every', every, digest(list));
}

// One odd object at each position of a short list: the miss at the first, a
// middle and the last iteration.
for (let at = 0; at < 5; at++) {
  for (let kind = 0; kind < 6; kind++) {
    const list = build(5, 0);
    list[at] = odd(at, kind);
    for (let r = 0; r < 12; r++) step(list, 9, 7);
    console.log('at', at, kind, digest(list));
  }
}

// The common layout again after all of it.
for (let r = 0; r < 10; r++) step(plain, 40, 30);
console.log('plain again', digest(plain));
