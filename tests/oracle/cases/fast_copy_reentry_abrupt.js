// Leaving a copied loop from its slow copy (lower/guard_region.h,
// "RE-ENTRY"): the iteration that missed a guard in the fast copy finishes in
// the slow copy, and from there the loop may break, continue, return or throw
// like any other. Each loop here is entered again afterwards.

class Gauge {
  constructor(v) { this._v = v; this._reads = 0; }
  get v() { return this._v; }
  set v(x) { this._v = x; }
}

class Trap {
  constructor(v, mode) { this._reads = 0; this._v = v; this.mode = mode; }
  get v() {
    this._reads++;
    if (this.mode === 'throw' && this._reads % 4 === 0) throw new RangeError('trap ' + this._reads);
    return this._v;
  }
  set v(x) {
    if (this.mode === 'throw-set') throw new TypeError('no store ' + x);
    this._v = x;
  }
}

class Box {
  constructor(g, tag) { this.g = g; this.tag = tag; this.n = 0; }
  get level() { return this.g.v; }
  set level(x) { this.g.v = x; }
}

function breaks(list, limit) {
  let s = 0, i = 0;
  for (; i < list.length; i++) {
    const b = list[i];
    const l = b.level;
    if (l > limit) break;
    b.level = l + 1;
    b.n++;
    if (b.tag === 'skip') continue;
    s += b.level * 2 + b.n;
  }
  return s + ':' + i;
}

function returns(list) {
  let s = 0;
  for (let i = 0; i < list.length; i++) {
    const b = list[i];
    b.level = b.level + 0.5;
    if (b.tag === 'stop') return 'stopped ' + i + ' ' + s;
    s += b.level;
  }
  return 'done ' + s;
}

function throws(list) {
  let s = 0, caught = 0;
  for (let r = 0; r < 6; r++) {
    try {
      for (let i = 0; i < list.length; i++) {
        const b = list[i];
        const before = b.level;
        b.level = before + 1;
        s += b.level - before + i;
      }
    } catch (e) {
      caught++;
      s += e.message.length;
    } finally {
      s += 0.125;
    }
  }
  return s + ' caught ' + caught;
}

function labelled(rows) {
  let s = 0;
  outer: for (let r = 0; r < rows.length; r++) {
    const row = rows[r];
    for (let i = 0; i < row.length; i++) {
      const b = row[i];
      const l = b.level;
      if (b.tag === 'next-row') continue outer;
      if (b.tag === 'all-done') break outer;
      b.level = l + 0.25;
      s += b.level + r;
    }
    s += 1000;
  }
  return s;
}

function boxes(n, at, make) {
  const list = [];
  for (let i = 0; i < n; i++) list.push(i === at ? make(i) : new Box(new Gauge(i % 6), 'plain'));
  return list;
}

// Warm with the plain layout.
const warm = boxes(40, -1, null);
for (let r = 0; r < 30; r++) breaks(warm, 1e9);
console.log('warm', breaks(warm, 1e9), returns(warm), throws(warm));

for (const at of [0, 1, 19, 38, 39]) {
  const skip = boxes(40, at, (i) => new Box(new Trap(i % 6, 'none'), 'skip'));
  console.log('continue', at, breaks(skip, 1e9), breaks(skip, 1e9));
  const high = boxes(40, at, (i) => new Box(new Trap(500, 'none'), 'plain'));
  console.log('break', at, breaks(high, 100), breaks(high, 100));
  const stop = boxes(40, at, (i) => new Box(new Trap(i, 'none'), 'stop'));
  console.log('return', at, returns(stop), returns(stop));
  const thrower = boxes(40, at, (i) => new Box(new Trap(i, 'throw'), 'plain'));
  console.log('throw', at, throws(thrower));
  const refuser = boxes(40, at, (i) => new Box(new Trap(i, 'throw-set'), 'plain'));
  console.log('throw-set', at, throws(refuser));
}

const rows = [];
for (let r = 0; r < 8; r++) {
  rows.push(boxes(12, r % 3 === 0 ? -1 : r, (i) => new Box(new Trap(i, 'none'), r === 6 ? 'all-done' : 'next-row')));
}
console.log('labelled', labelled(rows), labelled(rows));

// The plain loops once more.
console.log('again', breaks(warm, 1e9), returns(warm), throws(warm));
