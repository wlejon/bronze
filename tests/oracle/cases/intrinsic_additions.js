// Old libraries add methods to the builtin prototypes; with the intrinsics
// frozen, the addition is the realm's and every value of the kind has it.
Number.prototype.clamp = function (lo, hi) { return Math.min(Math.max(this, lo), hi); };
console.log((15).clamp(0, 10), (-3).clamp(0, 10), 4.5.clamp(0, 10));
const n = 7;
console.log(n.clamp(8, 9), typeof Number.prototype.clamp);

Object.prototype.clone = function () { return Object.assign({}, this); };
const o = { a: 1, b: { c: 2 } };
const c = o.clone();
console.log(c.a, c.b === o.b, c !== o);
console.log(typeof [].clone, typeof 'x'.clone, typeof (() => 1).clone);

Array.prototype.last = function () { return this[this.length - 1]; };
console.log([1, 2, 3].last(), [].last());

String.prototype.shout = function () { return this.toUpperCase() + '!'; };
console.log('hi'.shout(), `a${1}`.shout());

// A hot read site that saw the miss first, then the addition.
function readSeq(x) { return typeof x.seq; }
let misses = 0;
for (let i = 0; i < 20000; i++) if (readSeq([i]) === 'undefined') misses++;
Array.prototype.seq = function () { return 1; };
let hits = 0;
for (let i = 0; i < 20000; i++) if (readSeq([i]) === 'function') hits++;
console.log(misses, hits);

// An own property still shadows an addition; a write to an ordinary object
// makes its own.
const own = { clone: 5 };
console.log(own.clone);
const plain = {};
plain.last = 'mine';
console.log(plain.last, typeof [].last);

// Replacing an addition, and defineProperty of a new key.
Number.prototype.clamp = function () { return 'replaced'; };
console.log((1).clamp());
Object.defineProperty(Array.prototype, 'first', { value() { return this[0]; }, writable: true, configurable: true });
console.log([9, 8].first());

// Additions to a builtin function, to Function.prototype and to Math.
Function.prototype.method = function (name, fn) { this.prototype[name] = fn; return this; };
function Point(x) { this.x = x; }
Point.method('double', function () { return this.x * 2; });
console.log(new Point(4).double(), typeof Object.method);
Object.helper = function () { return 'helped'; };
console.log(Object.helper());
Math.clamp = function (v, lo, hi) { return Math.min(Math.max(v, lo), hi); };
console.log(Math.clamp(12, 0, 10));

// Babylon's array observer: a property on a builtin method, which it reads
// back to unhook.
function observe(arr, name, onChange) {
  const n = arr[name];
  const r = function () { const len = arr.length; const s = r.previous.apply(arr, arguments); onChange(name, len); return s; };
  n.next = r; r.previous = n; arr[name] = r;
  return () => { const i = r.previous; const nx = r.next; nx ? (i.next = nx, nx.previous = i) : (i.next = void 0, arr[name] = i); r.next = void 0; r.previous = void 0; };
}
const watched = [1];
const seen = [];
const unhook = observe(watched, 'push', (k, len) => seen.push(k + len));
watched.push(2, 3);
console.log(watched.length, seen.join(), Array.prototype.push.next === watched.push);
unhook();
watched.push(4);
console.log(watched.length, seen.length, Array.prototype.push.next, watched.push === Array.prototype.push);

// delete removes an addition; an index on Object.prototype fills holes.
String.prototype.gone = 1;
console.log('x'.gone, delete String.prototype.gone, 'x'.gone);
Object.prototype[3] = 'x';
Object.prototype[1] = 'y';
console.log([1][3], [1, , 3][1], ({})[3]);
console.log(3 in [1], [1, , 3].indexOf('y'), 'clone' in [], 'clone' in Object.create(null), 'last' in {});
delete Object.prototype[3];
delete Object.prototype[1];
console.log([1][3], [1, , 3][1]);

// A program that freezes an intrinsic itself gets the spec's refusal.
Object.freeze(Boolean.prototype);
try { Object.defineProperty(Boolean.prototype, 'flip', { value: 1 }); console.log('no throw'); }
catch (e) { console.log(e.constructor.name); }
(function () { 'use strict'; try { Boolean.prototype.flip = 1; console.log('no throw'); } catch (e) { console.log(e.constructor.name); } })();
console.log(typeof true.flip, Object.isFrozen(Boolean.prototype));

// Symbol keys, and a write through a strict function.
const tag = Symbol('tag');
(function () { 'use strict'; Object.prototype[tag] = 'tagged'; })();
console.log({}[tag]);
