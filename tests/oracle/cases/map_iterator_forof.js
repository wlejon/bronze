// for-of over a built-in ITERATOR OBJECT — `map.values()`, `set.keys()`,
// `arr.entries()` — steps the object's internal slots directly
// (runtime/iterator.h, MapIterator / ArrayIterator), so every observable
// consequence of the protocol has to survive that shortcut: the object is
// left where the loop stopped, a `return` added to it is called on `break`,
// a replaced `next` is honoured, and exhaustion latches.

const m = new Map([['a', 1], ['b', 2], ['c', 3], ['d', 4]]);

// Plain walks, every kind.
let out = [];
for (const v of m.values()) out.push(v);
for (const k of m.keys()) out.push(k);
for (const [k, v] of m.entries()) out.push(k + v);
for (const [k, v] of m) out.push(k + '=' + v);
console.log(out.join(','));

const s = new Set(['x', 'y', 'z']);
out = [];
for (const v of s.values()) out.push(v);
for (const v of s.keys()) out.push(v);
for (const [a, b] of s.entries()) out.push(a + b);
console.log(out.join(','));

// A break leaves the iterator object exactly one step past the last element
// the loop saw, and `it.next()` continues from there.
const it = m.values();
for (const v of it) { if (v === 2) break; }
console.log(JSON.stringify(it.next()), JSON.stringify(it.next()), JSON.stringify(it.next()));

// Exhaustion latches: a map that grows after the walk finished does not
// revive the iterator.
const it2 = m.keys();
for (const k of it2) {}
m.set('e', 5);
console.log(JSON.stringify(it2.next()));
// ...but one that was not finished sees the addition.
const it3 = m.keys();
it3.next();
m.set('f', 6);
out = [];
for (const k of it3) out.push(k);
console.log(out.join(','));

// A `return` method given to the iterator object after creation is called
// on an early exit, with the object as `this`.
const it4 = m.values();
let closed = 0;
it4.return = function () { closed++; console.log('return called, this is it4:', this === it4); return { done: true }; };
for (const v of it4) { if (v === 3) break; }
console.log('closed', closed);
// ...and not on normal completion.
const it5 = m.values();
it5.return = function () { console.log('MUST NOT PRINT'); return {}; };
for (const v of it5) {}
console.log('done without return');

// A replaced `next` on the object is what the loop calls.
const it6 = m.values();
let calls = 0;
it6.next = function () { calls++; return calls < 3 ? { value: 'n' + calls, done: false } : { value: undefined, done: true }; };
out = [];
for (const v of it6) out.push(v);
console.log(out.join(','), calls);

// A replaced [Symbol.iterator] on the object is honoured too.
const it7 = m.keys();
it7[Symbol.iterator] = function () { return ['p', 'q'][Symbol.iterator](); };
out = [];
for (const v of it7) out.push(v);
console.log(out.join(','));

// Spread, Array.from, destructuring, and a nested walk over the same map.
console.log([...m.values()].join(','), Array.from(m.keys()).join(''), Array.from(m.entries(), ([k, v]) => k + v).join(','));
const [first, second, ...rest] = m.values();
console.log(first, second, rest.join(','));
out = [];
for (const a of m.keys()) for (const b of m.keys()) if (a < b) out.push(a + b);
console.log(out.length, out.slice(0, 3).join(','));

// Array iterator objects: entries destructuring, keys, a break and resume,
// and a hole read as undefined.
const arr = ['A', , 'C'];
out = [];
for (const [i, v] of arr.entries()) out.push(i + ':' + v);
for (const i of arr.keys()) out.push(i);
console.log(out.join(','));
const ait = arr.values();
for (const v of ait) { if (v === undefined) break; }
console.log(JSON.stringify(ait.next()), JSON.stringify(ait.next()));
// An array that grows while its iterator is live is walked to its new end.
const grow = [1, 2];
out = [];
for (const v of grow.values()) { out.push(v); if (v === 2) grow.push(3); }
console.log(out.join(','));

// yield* over a built-in iterator: the delegated values, and the generator's
// early return reaching the iterator's `return` when it has one.
function* g() { yield* m.values(); }
out = [];
for (const v of g()) out.push(v);
console.log(out.join(','));
const it8 = m.values();
it8.return = function () { console.log('delegate closed'); return { done: true }; };
function* g2() { yield* it8; }
const gen = g2();
gen.next();
gen.return(0);
console.log(JSON.stringify(it8.next()));

// The result objects a manual walk sees keep the spec's field order.
console.log(Object.keys(m.values().next()).join(','), Object.keys([1].values().next()).join(','));
