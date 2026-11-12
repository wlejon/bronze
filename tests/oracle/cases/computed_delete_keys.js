'use strict';
// delete obj[key] for every kind of key and receiver.
const o = { a: 1, 2: 'two', '3.5': 'f', [Symbol.for('s')]: 'sym', 'long key with spaces': true };
const keys = ['a', 2, '2', 3.5, Symbol.for('s'), 'long key with spaces', 'absent', -0, null, undefined];
console.log(keys.map((k) => delete o[k]).join(), Object.keys(o).join(), Object.getOwnPropertySymbols(o).length);

// A key converted once, even when the property is absent.
let conversions = 0;
const key = { toString() { conversions++; return 'gone'; } };
const target = { gone: 1 };
console.log(delete target[key], delete target[key], conversions, 'gone' in target);

// Non-configurable properties refuse, and strict code throws naming them.
const fixed = Object.defineProperty({}, 'pinned', { value: 1, configurable: false });
Object.defineProperty(fixed, 7, { value: 1, configurable: false });
for (const k of ['pinned', 7]) {
    try { delete fixed[k]; console.log('deleted'); } catch (e) { console.log(e instanceof TypeError); }
}
const frozen = Object.freeze({ f: 1 });
try { delete frozen['f']; } catch (e) { console.log(e instanceof TypeError); }
console.log(delete frozen['other']);

// Arrays: elements become holes, length stays, `length` itself refuses.
const arr = [1, 2, 3, 4];
console.log(delete arr[1], delete arr['2'], delete arr[10], arr.length, 1 in arr, JSON.stringify(arr));
try { delete arr['length']; } catch (e) { console.log(e instanceof TypeError); }

// Classes: statics and methods go.
class K { static s = 1; m() {} }
console.log(delete K['s'], delete K.prototype['m'], K.s, typeof K.prototype.m);

// Primitives and strings.
console.log(delete (5)['x'], delete 'str'['x'], delete true['toString']);
for (const base of [null, undefined]) {
    try { delete base['k']; } catch (e) { console.log(e instanceof TypeError); }
}

// A cache keyed by computed strings and numbers, emptied key by key.
const cache = {};
for (let i = 0; i < 300; i++) { cache['k' + i] = i; cache[i] = -i; }
let removed = 0;
for (let i = 0; i < 300; i += 2) { if (delete cache['k' + i]) removed++; if (delete cache[i]) removed++; }
console.log(removed, Object.keys(cache).length, cache.k1, cache[1], cache.k2, cache[2]);

// Proxies see the trap with the converted key.
const seen = [];
const proxy = new Proxy({}, { deleteProperty(t, k) { seen.push(typeof k === 'symbol' ? 'symbol' : typeof k + ':' + k); return true; } });
delete proxy[1]; delete proxy['x']; delete proxy[Symbol.iterator]; delete proxy[1.5];
console.log(seen.join());
