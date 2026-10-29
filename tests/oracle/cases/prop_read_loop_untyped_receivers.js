// A property read in a loop whose receiver inference cannot type (an
// element of an array the caller filled) gets the inline cache in an AOT
// build: way 0 and the scan of the other ways at the site, the site's stub
// past them. Each phase warms the site on one population of receivers and
// then changes what it meets: new shapes, overflow slots, more shapes than
// the ways, prototype data and getters, deletes, accessors defined over a
// data property, frozen and dictionary-mode objects, receivers that are no
// plain object, and holes.

function sumX(arr) {
  let s = 0;
  for (let i = 0; i < arr.length; i++) s += arr[i].x;
  return s;
}

function listX(arr) {
  const out = [];
  for (let i = 0; i < arr.length; i++) out.push(String(arr[i].x));
  return out.join(',');
}

function fill(n, make) {
  const a = [];
  for (let i = 0; i < n; i++) a.push(make(i));
  return a;
}

// One shape, then the same values in shapes the site has not seen.
const lits = fill(1000, (i) => ({ x: i, y: -i }));
console.log('mono', sumX(lits), sumX(lits));
console.log('reordered', sumX(fill(1000, (i) => ({ y: 0, x: i }))));
console.log('overflow', sumX(fill(100, (i) => ({ a: 1, b: 2, c: 3, d: 4, e: 5, x: i }))));
console.log('extra', sumX(fill(100, (i) => (i % 2 ? { x: 1, z: 1 } : { x: 2 }))));

// Eight shapes cycling through one site: more than its ways.
const makers = [
  (i) => ({ x: i }),
  (i) => ({ a: 0, x: i }),
  (i) => ({ a: 0, b: 0, x: i }),
  (i) => ({ a: 0, b: 0, c: 0, x: i }),
  (i) => ({ a: 0, b: 0, c: 0, d: 0, x: i }),
  (i) => ({ q: 0, x: i }),
  (i) => ({ q: 0, r: 0, x: i }),
  (i) => ({ x: i, q: 0 }),
];
const mega = fill(800, (i) => makers[i % 8](i % 8));
console.log('mega', sumX(mega), sumX(mega));

// Inherited data, then an own property over it, then the prototype's value
// changed, then a getter replacing it on the prototype.
const proto = { x: 5 };
const kids = fill(100, () => Object.create(proto));
console.log('proto data', sumX(kids));
kids[3].x = 1000;
console.log('own shadow', sumX(kids));
proto.x = 7;
console.log('proto changed', sumX(kids));
let getterRuns = 0;
Object.defineProperty(proto, 'x', { get() { getterRuns++; return 1; }, configurable: true });
console.log('proto getter', sumX(kids), getterRuns);
delete kids[3].x;
console.log('own deleted', sumX(kids), getterRuns);
Object.setPrototypeOf(kids[0], { x: 50 });
console.log('proto swapped', sumX(kids), getterRuns);

// A data property turned into an accessor on the receiver itself, then
// deleted (undefined is NaN to the sum), then put back.
const objs = fill(10, (i) => ({ x: i, w: 0 }));
console.log('before define', sumX(objs), sumX(objs));
Object.defineProperty(objs[4], 'x', { get() { return 100; } });
console.log('own getter', sumX(objs));
delete objs[5].x;
console.log('deleted', sumX(objs));
objs[5].x = 5;
console.log('readded', sumX(objs));

// Frozen and dictionary-mode receivers.
const frozen = fill(10, (i) => Object.freeze({ x: i }));
console.log('frozen', sumX(frozen));
const dict = fill(10, (i) => {
  const o = { x: i };
  for (let k = 0; k < 40; k++) o['k' + k] = k;
  for (let k = 0; k < 40; k += 2) delete o['k' + k];
  return o;
});
console.log('dict', sumX(dict), sumX(dict));

// Receivers that are no plain object: an array, a function, a class
// instance with a getter, a typed array, a Map, primitives whose prototypes
// answer, and finally a hole, whose read throws.
class G { get x() { return 3; } }
const arrObj = [1, 2];
arrObj.x = 4;
const fnObj = function () {};
fnObj.x = 6;
const ta = new Float64Array(2);
ta.x = 8;
const map = new Map();
map.x = 9;
Number.prototype.x = 10;
String.prototype.x = 11;
Boolean.prototype.x = 12;
const odd = [{ x: 1 }, arrObj, fnObj, new G(), ta, map, 2, 'str', true, { x: 2 }];
console.log('kinds', listX(odd), sumX(odd));
delete Number.prototype.x;
delete String.prototype.x;
delete Boolean.prototype.x;
console.log('kinds after', listX(odd));
const holey = [{ x: 1 }, { x: 2 }];
holey[3] = { x: 4 };
try {
  console.log(sumX(holey));
} catch (e) {
  console.log('hole', e instanceof TypeError);
}
for (const bad of [null, undefined]) {
  try {
    sumX([{ x: 1 }, bad]);
  } catch (e) {
    console.log('nullish', e instanceof TypeError);
  }
}

// Receivers allocated as the loop runs (a collection can move them between
// the shape check and the load under GC stress).
let churn = 0;
for (let r = 0; r < 20; r++) {
  const fresh = fill(50, (i) => (i % 3 === 0 ? { x: i, s: 'a' + i } : { s: 'b', x: i }));
  churn += sumX(fresh);
}
console.log('churn', churn);
