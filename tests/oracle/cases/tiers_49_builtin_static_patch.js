// Assigning, deleting and redefining a static of a built-in constructor or
// namespace (`Array.isArray`, `Object.keys`, `Math.max`, `String.fromCharCode`)
// is observable: the next read, and the next call through a site that was warm
// on the original, sees the new value. Every probe below runs in a loop hot
// enough to tier up before each change.

function probeIsArray(n) {
  let hits = 0;
  for (let i = 0; i < n; i++) if (Array.isArray([i])) hits++;
  return hits;
}
function probeMax(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += Math.max(i, 1);
  return s;
}
function probeKeys(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += Object.keys({ a: i, b: 2 }).length;
  return s;
}
function probeFromCharCode(n) {
  let s = '';
  for (let i = 0; i < n; i++) s = String.fromCharCode(65 + (i % 3));
  return s;
}
function probeOf(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += Array.of(i, 1).length;
  return s;
}
function readIsArray() { return Array.isArray; }

// ---- warm every site on the originals -------------------------------------
for (let r = 0; r < 200; r++) {
  probeIsArray(50); probeMax(50); probeKeys(20); probeFromCharCode(10); readIsArray();
}
console.log('warm', probeIsArray(10), probeMax(10), probeKeys(10), probeFromCharCode(4));

const origIsArray = Array.isArray;
const origMax = Math.max;
const origKeys = Object.keys;
const origFromCharCode = String.fromCharCode;
const origOf = Array.of;

// ---- assignment -----------------------------------------------------------
Array.isArray = function (x) { return false; };
Math.max = function (a, b) { return -1; };
Object.keys = function (o) { return ['only']; };
String.fromCharCode = function () { return 'patched'; };
console.log('assigned', probeIsArray(10), probeMax(10), probeKeys(10), probeFromCharCode(4));
console.log('read', readIsArray() === Array.isArray, readIsArray() !== origIsArray);
{
  const d = Object.getOwnPropertyDescriptor(Array, 'isArray');
  console.log('desc', typeof d.value, d.writable, d.enumerable, d.configurable, d.value === Array.isArray);
}
console.log('names', Object.getOwnPropertyNames(Array).includes('isArray'), 'isArray' in Array,
            Array.hasOwnProperty('isArray'));

// ---- restore by assignment --------------------------------------------------
Array.isArray = origIsArray;
Math.max = origMax;
Object.keys = origKeys;
String.fromCharCode = origFromCharCode;
console.log('restored', probeIsArray(10), probeMax(10), probeKeys(10), probeFromCharCode(4));
console.log('same', Array.isArray === origIsArray, Object.keys === origKeys);

// ---- delete -----------------------------------------------------------------
console.log('delete', delete Array.isArray, delete Math.max, delete String.fromCharCode);
console.log('gone', typeof Array.isArray, typeof Math.max, typeof String.fromCharCode,
            'isArray' in Array, 'max' in Math);
try { probeIsArray(3); console.log('no throw'); } catch (e) { console.log('call deleted', e instanceof TypeError); }
try { probeMax(3); console.log('no throw'); } catch (e) { console.log('call deleted', e instanceof TypeError); }
console.log('names after delete', Object.getOwnPropertyNames(Array).includes('isArray'),
            Object.getOwnPropertyNames(Math).includes('max'));

// ---- defineProperty brings them back, with the attributes it names ----------
Object.defineProperty(Array, 'isArray', { value: origIsArray, writable: true, configurable: true, enumerable: false });
Object.defineProperty(Math, 'max', { value: function () { return 42; }, writable: false, configurable: true, enumerable: true });
String.fromCharCode = origFromCharCode;
console.log('defined', probeIsArray(10), probeMax(3), probeFromCharCode(4));
console.log('enum', Object.keys(Math).join(','));
Math.max = origMax;  // sloppy: a non-writable property ignores the write
console.log('still', Math.max(1, 2));
Object.defineProperty(Math, 'max', { value: origMax, writable: true, configurable: true, enumerable: false });
console.log('back', probeMax(10), Math.max(3, 9), Object.keys(Math).length);

// ---- a site warm on a static already written down, then patched -------------
for (let r = 0; r < 200; r++) probeOf(50);
console.log('of warm', probeOf(10));
Array.of = function () { return [0]; };
console.log('of patched', probeOf(10));
Array.of = origOf;
console.log('of restored', probeOf(10));

// ---- a subclass reads the base's statics through its own chain --------------
class MyArr extends Array {}
for (let r = 0; r < 200; r++) MyArr.isArray([r]);
Array.isArray = function () { return 'via base'; };
console.log('subclass', MyArr.isArray([]), MyArr.of(1, 2) instanceof MyArr);
Array.isArray = origIsArray;
console.log('subclass restored', MyArr.isArray([]));

// ---- a getter installed over a static runs on every read --------------------
let reads = 0;
Object.defineProperty(Array, 'from', { get() { reads++; return () => 'got'; }, configurable: true });
let got = '';
for (let i = 0; i < 5; i++) got = Array.from([1]);
console.log('getter', got, reads);

// ---- a polyfill for a static the constructor lacks is the program's to add --
Number.polyfilled = function (x) { return x * 2; };
console.log('polyfill', Number.polyfilled(21), typeof Number.isInteger);

// ---- nothing else moved ------------------------------------------------------
console.log('others', Array.of(1, 2).length, Number.isInteger(3), JSON.stringify({ a: 1 }),
            Array.isArray([]), String.fromCharCode(66), Object.keys({ x: 1, y: 2 }).join(''));
