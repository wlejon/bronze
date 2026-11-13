// Copied loops inside loops (lower/guard_region.h, "RE-ENTRY"): an inner
// loop whose fast copy is left and entered again while the outer loop's
// accessors go on, an outer loop whose miss happens between two runs of the
// inner one, and an inner loop that changes the objects the outer one reads.

class Vec {
  constructor(x, y) { this._x = x; this._y = y; }
  get x() { return this._x; }
  set x(v) { this._x = v; }
  get y() { return this._y; }
  set y(v) { this._y = v; }
}

class Odd {
  constructor(x, y) { this._y = y; this._x = x; this.reads = 0; }
  get x() { this.reads++; return this._x; }
  set x(v) { this._x = v; }
  get y() { return this._y + 0.5; }
  set y(v) { this._y = v - 0.5; }
}

class Group {
  constructor(id, items) {
    this.id = id;
    this._origin = new Vec(id, -id);
    this.items = items;
    this.weight = 1 + (id % 3) * 0.5;
  }
  get ox() { return this._origin.x; }
  set ox(v) { this._origin.x = v; }
  get oy() { return this._origin.y; }
}

function sweep(groups) {
  let s = 0;
  for (let g = 0; g < groups.length; g++) {
    const group = groups[g];
    const items = group.items;
    let inner = 0;
    for (let i = 0; i < items.length; i++) {
      const it = items[i];
      it.x = it.x + group.ox * 0.25;
      it.y = it.y - group.oy * 0.125;
      inner += it.x * group.weight + it.y;
    }
    group.ox = group.ox + inner * 1e-3;
    for (let i = 0; i < items.length; i++) {
      const it = items[i];
      inner += it.x - it.y;
    }
    s += inner + group.ox + group.oy;
  }
  return s;
}

// An inner loop that swaps the outer object's origin every few items.
function churn(groups) {
  let s = 0;
  for (let g = 0; g < groups.length; g++) {
    const group = groups[g];
    const before = group.ox;
    for (let i = 0; i < group.items.length; i++) {
      const it = group.items[i];
      if ((i + g) % 4 === 0) group._origin = (i % 8 === 0) ? new Odd(group.ox + 1, group.oy) : new Vec(group.ox + 1, group.oy);
      it.x += group.ox;
      s += it.x + it.y;
    }
    s += group.ox - before;
  }
  return s;
}

function make(nGroups, nItems, oddItem, oddGroup) {
  const groups = [];
  for (let g = 0; g < nGroups; g++) {
    const items = [];
    for (let i = 0; i < nItems; i++) {
      items.push(oddItem(g, i) ? new Odd(i, g) : new Vec(i, g));
    }
    const group = new Group(g, items);
    if (oddGroup(g)) group._origin = new Odd(g, -g);
    groups.push(group);
  }
  return groups;
}

const never = () => false;
const plain = make(12, 16, never, never);
let t = 0;
for (let r = 0; r < 30; r++) t += sweep(plain);
console.log('plain', t.toFixed(4));

const variants = [
  ['items-some', (g, i) => (g * 16 + i) % 7 === 3, never],
  ['items-all', () => true, never],
  ['items-last', (g, i) => i === 15, never],
  ['items-first', (g, i) => i === 0, never],
  ['groups-some', never, (g) => g % 3 === 1],
  ['groups-all', never, () => true],
  ['both', (g, i) => (g + i) % 5 === 0, (g) => g % 2 === 0],
  ['one-group', (g, i) => g === 5, (g) => g === 5],
];
for (const [name, oddItem, oddGroup] of variants) {
  const groups = make(12, 16, oddItem, oddGroup);
  let v = 0;
  for (let r = 0; r < 20; r++) v += sweep(groups);
  let reads = 0;
  for (const group of groups) {
    if (group._origin.reads) reads += group._origin.reads;
    for (const it of group.items) if (it.reads) reads += it.reads;
  }
  console.log(name, v.toFixed(4), reads);
}

const churned = make(10, 12, (g, i) => i % 5 === 2, never);
let c = 0;
for (let r = 0; r < 15; r++) c += churn(churned);
console.log('churn', c.toFixed(4));

// Empty and single inner loops between full ones.
const ragged = make(9, 0, never, never);
for (let g = 0; g < ragged.length; g++) {
  for (let i = 0; i < g % 4; i++) ragged[g].items.push(i % 2 === 0 ? new Vec(i, g) : new Odd(i, g));
}
let rg = 0;
for (let r = 0; r < 25; r++) rg += sweep(ragged) + churn(ragged);
console.log('ragged', rg.toFixed(4));
