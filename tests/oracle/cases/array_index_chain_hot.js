// Code compiled hot while no prototype holds an index key, which a prototype
// then gains: the dense read, the read run, the counted loop, the indexed
// store, push and shift must all see the inherited accessor from then on
// (the element fast paths stay compiled; what changes is that their hole and
// past-the-end exits now walk the chain). After the delete the chain holds
// nothing again and the answers go back.
// Hot code compiled while no chain holds an index, then a prototype gains one.
function readHole(a) { return a[1]; }
function readRun(a) { const x = a[0], y = a[1], z = a[2]; return [x, y, z]; }
function sumNums(a) { let s = 0; for (let i = 0; i < a.length; i++) { const v = a[i]; s += typeof v === 'number' ? v : 100; } return s; }
function writeAt(a, i, v) { a[i] = v; return a; }
function pushOne(a, v) { a.push(v); return a.length; }
function shiftOne(a) { return a.shift(); }
function fillRun(a) { a[0] = 1; a[1] = 2; a[2] = 3; a[3] = 4; return a; }
function matrix(a) { const t = a[0] * 2 + a[1] * 3 + a[2]; return t; }
const holey = () => { const a = [10, 20, 30, 40]; delete a[1]; return a; };
let acc = 0;
for (let i = 0; i < 20000; i++) {
  acc += readHole(holey()) === undefined ? 1 : 0;
  acc += readRun(holey())[1] === undefined ? 1 : 0;
  acc += sumNums(holey());
  writeAt(holey(), 1, 5);
  pushOne([1, 2], 3);
  shiftOne([, 2]);
  fillRun(holey());
  acc += matrix([1, 2, 3]);
  acc += Number.isNaN(matrix(holey())) ? 1 : 0;
}
console.log('warm', acc);
let log = '';
Object.defineProperty(Object.prototype, 1, {
  get() { log += 'get1,'; return 7; },
  set(v) { log += 'set1=' + v + ','; },
  configurable: true,
});
console.log('readHole', readHole(holey()));
{ const r = readRun(holey()); console.log('readRun', r[0], r[1], r[2]); }
console.log('sumNums', sumNums(holey()));
const w = writeAt(holey(), 1, 5);
console.log('writeAt', Object.hasOwn(w, 1), w.length);
const pa = [9];
console.log('push', pushOne(pa, 3), Object.hasOwn(pa, 1), pa.length);
console.log('shift', shiftOne([0, , 2]));
const fr = fillRun(holey());
console.log('fillRun', Object.hasOwn(fr, 1), fr[0], fr[2], fr[3]);
console.log('matrix', matrix(holey()));
console.log('log', log);
delete Object.prototype[1];
console.log('after delete', readHole(holey()), sumNums(holey()));
