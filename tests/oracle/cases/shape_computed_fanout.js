// Objects given keys nobody else has. The transition tree grows a node for a
// computed key only up to a bound per node and per thread (Shape::
// refusesNewTransition); past it the object becomes a dictionary instead. Every
// answer must be the same either side of the bound, and objects built from key
// constants — literals, constructors, classes — keep working as before.

// `{}` given one fresh name each, far past the per-node bound.
let count = 0;
let sum = 0;
const kept = [];
for (let i = 0; i < 9000; i++) {
  const t = {};
  const k = 'fresh_' + i;
  t[k] = i;
  t.after = i * 2;
  count += Object.keys(t).length;
  sum += t[k] + t.after;
  if (i % 3000 === 0) kept.push(t);
}
console.log('fresh', count, sum);
for (const t of kept) console.log(' kept', JSON.stringify(t), Object.keys(t).join());

// A shared first key, then fresh ones: the bound applies at the shared node.
const objs = [];
for (let i = 0; i < 5000; i++) {
  const o = { kind: 'rec' };
  o['field_' + i] = i;
  o['second_' + i] = -i;
  objs.push(o);
}
let check = 0;
for (let i = 0; i < objs.length; i++) check += objs[i]['field_' + i] - objs[i]['second_' + i] + objs[i].kind.length;
console.log('shared', check, JSON.stringify(objs[0]), JSON.stringify(objs[4999]));
delete objs[5].kind;
console.log('shared-delete', JSON.stringify(objs[5]), 'kind' in objs[5], objs[6].kind);

// Accessors and definitions with fresh names past the bound.
for (let i = 0; i < 4500; i++) {
  const o = {};
  Object.defineProperty(o, 'acc_' + i, { get() { return i; }, enumerable: true, configurable: true });
  Object.defineProperty(o, 'val_' + i, { value: i + 1, enumerable: false, writable: false });
  if (i % 1500 === 0) {
    const d = Object.getOwnPropertyDescriptor(o, 'val_' + i);
    console.log('define', i, o['acc_' + i], o['val_' + i], Object.keys(o).join(), d.writable, d.enumerable);
  }
}

// Objects built from key constants still share shapes and read the same.
class Point {
  constructor(x, y) { this.x = x; this.y = y; }
  len() { return Math.sqrt(this.x * this.x + this.y * this.y); }
}
let lens = 0;
for (let i = 0; i < 2000; i++) {
  const p = new Point(i, i + 1);
  const lit = { a: i, b: 'x', c: [i] };
  lens += p.len() > i ? 1 : 0;
  lens += lit.a === i && lit.c[0] === i ? 1 : 0;
}
console.log('constants', lens);

// JSON records of one layout after many of distinct layouts: the hundredth of
// a layout follows the first one's nodes.
const parsed = [];
for (let i = 0; i < 5000; i++) parsed.push(JSON.parse('{"u' + i + '": ' + i + ', "v": 1}'));
const same = [];
for (let i = 0; i < 50; i++) same.push(JSON.parse('{"id": ' + i + ', "name": "n' + i + '", "tags": ["a"]}'));
let total = 0;
for (const r of same) total += r.id + r.name.length + r.tags.length;
let total2 = 0;
for (let i = 0; i < parsed.length; i++) total2 += parsed[i]['u' + i] + parsed[i].v;
console.log('json', total, total2, Object.keys(parsed[4999]).join(), JSON.stringify(same[49]));

// Spread, assign and for-in over objects on either side of the bound.
const before = {};
before['p_' + 1] = 1;
const many = [];
for (let i = 0; i < 5000; i++) { const o = {}; o['q_' + i] = i; many.push(o); }
const merged = Object.assign({}, before, many[4999], { z: 0 });
const spread = { ...many[0], ...many[4999] };
const fk = [];
for (const k in merged) fk.push(k);
console.log('merge', JSON.stringify(merged), JSON.stringify(spread), fk.join());
