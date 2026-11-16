// Many keys absent from one receiver shape, asked at sites of their own
// (three.js asks a material for half a dozen `isXMaterial` flags a draw),
// over receivers of more shapes than member cells cover, so each speculated
// read tests the shape's absent cache (shape.lacks). The cache holds a few
// entries per shape, each key in one of two buckets: twenty-four keys
// overfill it, so entries are evicted and moved between their buckets while
// the answers must stay `undefined`. Then the answers change: a shared
// prototype gains some of the keys (a data property, a getter), one kind
// gains one as its own, and the prototype loses them again.
class Base { constructor(i) { this.i = i; } }
const kinds = [];
for (let k = 0; k < 9; k++) {
  kinds.push(class extends Base { constructor(i) { super(i); this['f' + k] = k; } });
}
function probe(o) {
  let n = 0;
  if (o.k00 !== undefined) n += 1; if (o.k01 !== undefined) n += 2; if (o.k02 !== undefined) n += 4;
  if (o.k03 !== undefined) n += 8; if (o.k04 !== undefined) n += 16; if (o.k05 !== undefined) n += 32;
  if (o.k06 !== undefined) n += 64; if (o.k07 !== undefined) n += 128; if (o.k08 !== undefined) n += 256;
  if (o.k09 !== undefined) n += 512; if (o.k10 !== undefined) n += 1024; if (o.k11 !== undefined) n += 2048;
  if (o.k12 !== undefined) n += 4096; if (o.k13 !== undefined) n += 8192; if (o.k14 !== undefined) n += 16384;
  if (o.k15 !== undefined) n += 32768; if (o.k16 !== undefined) n += 65536; if (o.k17 !== undefined) n += 131072;
  if (o.k18 !== undefined) n += 262144; if (o.k19 !== undefined) n += 524288; if (o.k20 !== undefined) n += 1048576;
  if (o.k21 !== undefined) n += 2097152; if (o.k22 !== undefined) n += 4194304; if (o.k23 !== undefined) n += 8388608;
  return n;
}
function few(o) {
  return (o.k03 === undefined ? 1 : 10) + (o.k19 === undefined ? 1 : 100);
}
function walk(list) {
  let s = 0;
  for (let k = 0; k < list.length; k++) s += probe(list[k]) + few(list[k]);
  return s;
}
const list = [];
for (let i = 0; i < 36; i++) list.push(new kinds[i % kinds.length](i));
let total = 0;
for (let r = 0; r < 3000; r++) total += walk(list);
console.log('warm', total);

const out = [];
Base.prototype.k05 = 5;
out.push(walk(list));
Object.defineProperty(Base.prototype, 'k19', { get() { return 19; }, configurable: true });
out.push(walk(list));
kinds[2].prototype.k11 = 11;
out.push(walk(list));
list[4].k22 = 'own';
out.push(walk(list));
delete Base.prototype.k05;
delete Base.prototype.k19;
delete kinds[2].prototype.k11;
out.push(walk(list));
console.log(out.join());
for (let r = 0; r < 3000; r++) total += walk(list);
console.log('end', total);
