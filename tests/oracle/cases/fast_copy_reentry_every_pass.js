// A site of a copied loop that misses on every iteration: the fast copy is
// left and entered again each time round (lower/guard_region.h, "RE-ENTRY"),
// and the answers are the slow copy's. The receivers alternate between two
// layouts with the same keys (a field that holds a small integer in one and a
// fraction in the other, a field added in another order), and between
// classes that answer the same accessor.

class Cell {
  constructor(v) { this._v = v; this._n = 0; }
  get v() { return this._v; }
  set v(x) { this._v = x; this._n++; }
}

class Other {
  constructor(v) { this._n = 0; this._v = v; }
  get v() { return this._v * 2; }
  set v(x) { this._v = x / 2; this._n += 2; }
}

class Holder {
  constructor(i, cell) {
    this.i = i;
    this.cell = cell;
    this.sum = 0;
  }
  get value() { return this.cell.v; }
  set value(x) { this.cell.v = x; }
}

function run(list, rounds) {
  let total = 0;
  for (let r = 0; r < rounds; r++) {
    for (let i = 0; i < list.length; i++) {
      const h = list[i];
      const a = h.value;
      h.value = a + 1;
      h.sum += h.value - a;
      total += h.value + h.sum * 0.5;
    }
  }
  return total;
}

function counts(list) {
  let n = 0;
  for (const h of list) n += h.cell._n;
  return n;
}

function make(n, pick) {
  const list = [];
  for (let i = 0; i < n; i++) list.push(new Holder(i, pick(i)));
  return list;
}

// One layout throughout.
const mono = make(50, (i) => new Cell(i));
console.log('mono', run(mono, 40), counts(mono));

// Integers and fractions in the same field: two layouts under one key list.
const reprs = make(50, (i) => new Cell(i % 2 === 0 ? i : i + 0.5));
console.log('reprs', run(reprs, 40), counts(reprs));

// Two classes behind one accessor, alternating, then in runs.
const classes = make(50, (i) => (i % 2 === 0 ? new Cell(i) : new Other(i)));
console.log('classes', run(classes, 40), counts(classes));
const runs = make(50, (i) => (i % 10 < 5 ? new Cell(i) : new Other(i)));
console.log('runs', run(runs, 40), counts(runs));

// Every holder its own layout.
const each = make(40, (i) => {
  const c = new Cell(i);
  c['k' + i] = i;
  return c;
});
console.log('each', run(each, 30), counts(each));

// The holder itself is what differs: the cell field at another slot.
const holders = [];
for (let i = 0; i < 40; i++) {
  if (i % 2 === 0) {
    holders.push(new Holder(i, new Cell(i)));
  } else {
    const h = Object.create(Holder.prototype);
    h.sum = 0; h.cell = new Cell(i); h.i = i;
    holders.push(h);
  }
}
console.log('holders', run(holders, 30), counts(holders));

// A single element, so that every pass is the first and the last.
console.log('one', run(make(1, () => new Other(3)), 200));
console.log('none', run([], 5));
