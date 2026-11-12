// An inlined method body's guard asks a cell pair that holds one shape, and
// the site's method cache when the pair says no. A cache hit teaches the pair
// the receiver's shape (codegen-brass/il2mir/il_accessor_direct.h,
// `emit_method_cell_learn`), so a site whose first receiver was a rare one
// does not leave every later one on the cache. Each block gives a site a pair
// that holds the wrong shape, or none, and then what a pair that learned too
// much would answer wrongly.

class Item {
  constructor(v) {
    this.v = v;
    this.hits = 0;
  }
  touch(d) {
    this.hits++;
    this.v += d;
    return this.v;
  }
  size() {
    return this.v * 2;
  }
}
// The same methods on another shape: one more field, set first.
class Root extends Item {
  constructor(v) {
    super(v);
    this.root = true;
  }
}
class Loud extends Item {
  touch(d) {
    this.hits += 10;
    this.v -= d;
    return -this.v;
  }
}

function step(o, d) {
  return o.touch(d) + o.size();
}
function sweep(list, d) {
  let acc = 0;
  for (let i = 0; i < list.length; i++) acc += step(list[i], d);
  return acc;
}
function tally(list) {
  let hits = 0;
  let v = 0;
  for (const o of list) {
    hits += o.hits;
    v += o.v;
  }
  return hits + ' ' + v;
}

// 1. the rare receiver first, then many of the common shape.
const root = new Root(1);
const items = [];
for (let i = 0; i < 400; i++) items.push(new Item(i % 7));
let acc = step(root, 1);
for (let r = 0; r < 60; r++) acc += sweep(items, 1) + step(root, 2);
console.log('1 ' + acc + ' | ' + tally(items) + ' | ' + tally([root]));

// 2. the two shapes alternating, and in runs.
const both = [];
for (let i = 0; i < 300; i++) both.push(i & 1 ? new Root(i) : new Item(i));
for (let i = 0; i < 100; i++) both.push(i < 50 ? new Root(i) : new Item(i));
acc = 0;
for (let r = 0; r < 40; r++) acc += sweep(both, r & 3);
console.log('2 ' + acc + ' | ' + tally(both));

// 3. an override among receivers the pair has learned.
const withLoud = items.slice(0, 20);
withLoud.splice(7, 0, new Loud(5));
withLoud.push(new Loud(6), root);
acc = 0;
for (let r = 0; r < 30; r++) acc += sweep(withLoud, 3);
console.log('3 ' + acc + ' | ' + tally(withLoud));

// 4. an own property of the method's name on a learned shape's sibling, then
// on the learned receiver itself.
const own = new Item(2);
own.touch = function (d) {
  this.hits += 100;
  return d;
};
acc = step(own, 4) + step(items[0], 4) + step(own, 4);
root.size = () => 'own';
console.log('4 ' + acc + ' ' + step(root, 1) + ' ' + step(items[1], 1) + ' | ' + tally([own, root]));
delete root.size;
console.log('4b ' + step(root, 1));

// 5. the method replaced on the prototype: the pair's epoch moves, the first
// receiver after it is the rare one again, and the common shape follows.
const origTouch = Item.prototype.touch;
Item.prototype.touch = function (d) {
  this.hits += 2;
  this.v += d * 2;
  return this.v;
};
acc = step(root, 1);
for (let r = 0; r < 30; r++) acc += sweep(items, 1) + step(root, 1);
console.log('5 ' + acc + ' | ' + tally(items) + ' | ' + tally([root]));
Item.prototype.touch = origTouch;
acc = step(root, 1);
for (let r = 0; r < 30; r++) acc += sweep(items, 1) + step(root, 1);
console.log('5b ' + acc + ' | ' + tally(items) + ' | ' + tally([root]));

// 6. the method shadowed on the subclass prototype after the pair learned the
// subclass shape, with the base shape still passing through.
Root.prototype.size = function () {
  return 'R' + this.v;
};
console.log('6 ' + step(root, 1) + ' ' + step(items[2], 1) + ' ' + step(new Root(3), 1));
delete Root.prototype.size;
acc = 0;
for (let r = 0; r < 20; r++) acc += step(root, 1) + step(items[r], 1);
console.log('6b ' + acc);

// 7. a receiver that is no plain object first after an epoch move (a pair
// that refuses), then the plain ones; and a receiver whose prototype is
// swapped away from the class.
Item.prototype.extra = 1;
const proxied = new Proxy(new Item(9), {});
acc = step(proxied, 1);
for (let r = 0; r < 20; r++) acc += sweep(items, 1) + step(proxied, 1) + step(root, 1);
console.log('7 ' + acc + ' | ' + tally(items));
const swapped = new Item(4);
acc = step(swapped, 1);
Object.setPrototypeOf(swapped, {
  touch() {
    return 'far';
  },
  size() {
    return 'away';
  },
});
console.log('7b ' + acc + ' ' + step(swapped, 1) + ' ' + step(items[3], 1) + ' ' + step(root, 1));

// 8. a method that changes its receiver's shape, so the second guard of one
// call sees a shape the pair has not held.
Item.prototype.touch = function (d) {
  if (this.grown === undefined) this.grown = d;
  this.hits++;
  return this.v;
};
const fresh = [];
for (let i = 0; i < 50; i++) fresh.push(i % 3 ? new Item(i) : new Root(i));
acc = 0;
for (let r = 0; r < 10; r++) acc += sweep(fresh, r) + sweep(items, r);
console.log('8 ' + acc + ' | ' + tally(fresh) + ' | ' + tally(items));
