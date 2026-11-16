// Reads of a key most receivers lack, at sites whose receivers come in more
// shapes than member cells cover (a full cache) or whose ways found the key
// own on some shapes and nowhere on others: the speculated read tests the
// shape's own slot and the shape's absent cache (shape.lacks), which the
// runtime fills under the prototype epoch. After a long training phase the
// same sites meet every way of changing the answer: receivers that are no
// plain objects (a proxy, an array, a typed array, a function, primitives),
// a dictionary-mode receiver, a null-prototype one, a receiver whose
// prototype was swapped, one given the key as its own; then the shared
// prototypes gain the key (a data property, then a getter) and lose it
// again. A harvest reads the ways the caches hold at the end, and a full
// cache keeps the shapes it latched first (the first four kinds, under Mid),
// so the answers change only for the kinds under Mid2, which the caches
// never held, and the odd receivers come before any prototype change (whose
// epoch bump frees the ways for refilling).
class Base { constructor(i) { this.i = i; } }
class Mid extends Base {}
class Mid2 extends Mid {}
const kinds = [];
for (let k = 0; k < 9; k++) {
  const B = k < 4 ? Mid : Mid2;
  kinds.push(class extends B { constructor(i) { super(i); this['f' + k] = k; } });
}
class Owner extends Base { constructor(i) { super(i); this.isGroup = 'own'; } }
function probe(o) { return o.isGroup; }
function probeMixed(o) { return o.isLight; }
function walk(list) {
  let s = '';
  for (let k = 0; k < list.length; k++) {
    const v = probe(list[k]);
    const w = probeMixed(list[k]);
    s += (v === undefined ? '.' : String(v)[0]) + (w === undefined ? '-' : String(w)[0]);
  }
  return s;
}
function fill(list) {
  for (let i = 0; i < 27; i++) list.push(new kinds[i % kinds.length](i));
  for (let i = 0; i < 3; i++) { const o = new Owner(i); o.isLight = 'L'; list.push(o); }
  return list;
}
const list = fill([]);
let hash = 0;
const mix = (str) => { for (let c = 0; c < str.length; c++) hash = (hash * 31 + str.charCodeAt(c)) | 0; };
for (let r = 0; r < 4000; r++) mix(walk(list));
console.log('warm', hash, walk(list));

const both = (o) => String(probe(o)) + ' ' + String(probeMixed(o));
const dict = new kinds[2](2);
for (let k = 0; k < 40; k++) dict['x' + k] = k;
for (let k = 0; k < 40; k++) delete dict['x' + k];
console.log('dict', both(dict));
dict.isGroup = 'dict own';
console.log('dict own', both(dict));
console.log('null proto', both(Object.create(null)));
console.log('proxy', both(new Proxy({}, { get: (t, k) => 'trap:' + String(k) })));
const fn = function () {};
fn.isLight = 'fn';
console.log('others', both([1, 2]), both(new Uint8Array(2)), both(fn), both('str'), both(7), both(true));
const swapped = new kinds[4](4);
Object.setPrototypeOf(swapped, { isGroup: 'swapped', isLight: 'sw' });
const owned = new kinds[5](5);
owned.isGroup = 'late own';
console.log('swapped', both(swapped), 'own add', both(owned));

Mid2.prototype.isGroup = 'mid';
console.log('mid data', walk(list), both(swapped), both(owned));
delete Mid2.prototype.isGroup;
Mid.prototype.unrelated = 1;
console.log('deleted', walk(list));
Object.defineProperty(Mid2.prototype, 'isLight', { get() { return 'getter'; }, configurable: true });
console.log('base getter', walk(list), both(swapped));
delete Mid2.prototype.isLight;
delete Mid.prototype.unrelated;
console.log('getter gone', walk(list), both(owned));
for (let r = 0; r < 4000; r++) mix(walk(list));
console.log('end', hash, walk(list));
