// Warm bare-name reads and writes of globals while what they read through
// moves or changes: collections in the loop, the global object's slots
// growing, a property deleted and made again, made read-only, and frozen.
globalThis.counter = 0;
function bump() { counter = counter + 1; return counter; }
function read() { return counter; }

// Collections mid-loop: every iteration allocates.
let keep = [];
for (let i = 0; i < 200000; i++) {
  bump();
  keep.push({ i, s: 'x' + i });
  if (keep.length > 1000) keep = [];
}
console.log(read(), counter);

// The slots grow while the sites are warm.
for (let i = 0; i < 300; i++) {
  globalThis['grow' + i] = i;
  bump();
}
console.log(read(), grow299);

// Deleted, then made again.
delete globalThis.counter;
console.log(typeof counter);
try { read(); } catch (e) { console.log(e.constructor.name); }
globalThis.counter = 10;
for (let i = 0; i < 1000; i++) bump();
console.log(read());

// Read-only: a sloppy write is dropped, a strict one throws.
Object.defineProperty(globalThis, 'counter', { writable: false });
bump();
console.log(read());
(function () {
  'use strict';
  try { counter = 5; } catch (e) { console.log('strict', e.constructor.name); }
})();
Object.defineProperty(globalThis, 'counter', { writable: true });
bump();
console.log(read());

// A global that is a function, called hot, then replaced.
globalThis.f = function () { return 1; };
function callF() { let s = 0; for (let i = 0; i < 1000; i++) s += f(); return s; }
console.log(callF());
f = function () { return 2; };
console.log(callF());

// Frozen: writes dropped, reads still right.
Object.freeze(globalThis);
bump();
console.log(read(), Object.isFrozen(globalThis));
