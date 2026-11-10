// Object.hasOwn and hasOwnProperty with computed, numeric, symbol and
// throwing keys, on plain, dictionary, array, function and string receivers.
'use strict';
const sym = Symbol('s');
const o = { a: 1, 2: 'two', [sym]: 3 };
const dict = { x: 1, y: 2 };
delete dict.x;
const arr = [1, 2];
arr.extra = true;
function fn() {}
fn.stat = 1;
const keys = ['a', 'b', 2, '2', sym, 'x', 'y', 'extra', 0, 'length', 'stat', 'prototype', 'name', 'z' + 1];
for (const target of [o, dict, arr, fn, Object('str')]) {
  console.log(keys.map(k => (Object.hasOwn(target, k) ? 1 : 0) + '' + (Object.prototype.hasOwnProperty.call(target, k) ? 1 : 0)).join(' '));
}
const keyObj = { toString() { return 'a'; } };
console.log(Object.hasOwn(o, keyObj), Object.hasOwn(o, { toString() { return 'nope'; } }));
try { Object.hasOwn(o, { toString() { throw new Error('boom'); } }); } catch (e) { console.log('threw', e.message); }
try { Object.hasOwn(null, 'a'); } catch (e) { console.log('null', e.constructor.name); }
let hits = 0;
for (let i = 0; i < 20000; i++) if (Object.hasOwn(o, 'q' + i) || Object.hasOwn(o, i === 7 ? 'a' : 'r' + i)) hits++;
console.log('fresh', hits);
