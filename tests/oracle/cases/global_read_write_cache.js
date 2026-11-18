// Bare-name reads and writes of the global object's properties are cached
// per realm; every way the property can change under a cache must be seen.
globalThis.g = 1;
function readG() { return g; }
function writeG(v) { g = v; }
let s = 0;
for (let i = 0; i < 1000; i++) { writeG(i); s += readG(); }
console.log(s, g, globalThis.g);

// Redefined read-only in place: the cached write must stop (sloppy: no-op).
Object.defineProperty(globalThis, 'g', { writable: false });
writeG(5);
console.log(readG(), Object.getOwnPropertyDescriptor(globalThis, 'g').writable);
(function () { 'use strict'; try { g = 6; } catch (e) { console.log(e.constructor.name); } })();

// Redefined as an accessor.
globalThis.h = 1;
function readH() { return h; }
for (let i = 0; i < 100; i++) readH();
let calls = 0;
Object.defineProperty(globalThis, 'h', { get() { calls++; return 'got'; }, configurable: true });
console.log(readH(), readH(), calls);

// Deleted and re-added: a different slot.
globalThis.k = 'one';
globalThis.k2 = 'other';
function readK() { return typeof k === 'undefined' ? 'gone' : k; }
for (let i = 0; i < 100; i++) readK();
delete globalThis.k;
console.log(readK());
globalThis.k = 'two';
console.log(readK(), k2);

// A builtin's binding overwritten and put back.
const realJSON = JSON;
function readJSON() { return JSON; }
for (let i = 0; i < 100; i++) readJSON();
JSON = { stringify: () => 'mine' };
console.log(readJSON().stringify(1), JSON.stringify(1));
JSON = realJSON;
console.log(readJSON().stringify([1]));

// Writes from many sites to a sloppy-created global.
function bump() { counter = (typeof counter === 'number' ? counter : 0) + 1; }
for (let i = 0; i < 500; i++) bump();
console.log(counter, globalThis.counter, Object.keys(globalThis).includes('counter'));

// Frozen global object: the cached write must stop.
globalThis.frozenAfter = 1;
function writeF(v) { frozenAfter = v; }
for (let i = 0; i < 100; i++) writeF(i);
console.log(frozenAfter);
