// Array.prototype.values / keys / entries (ECMA-262 23.1.3.34, 23.1.3.17,
// 23.1.3.5) and the ArrayIterator they hand back (23.1.5) — with the identity
// 23.1.3.41 pins: `[Symbol.iterator]` IS `values`, one function object.
//
// Derived from ECMA-262:
//
// 1. 23.1.3.41: `Array.prototype[Symbol.iterator]` is the SAME function
//    object as `Array.prototype.values`, so every `===` below holds — on the
//    array, on the prototype, and across the two.
// 2. 23.1.5.2.1: values in index order, then `{ value: undefined,
//    done: true }`, and an exhausted iterator STAYS done.
// 3. `keys` yields the indices and `entries` yields [index, value] pairs,
//    which destructure in a for-of.
// 4. 27.1.2.1: `%IteratorPrototype%[Symbol.iterator]` returns `this`, which
//    an array iterator inherits — an iterator is its own iterable.
// 5. 23.1.5.2.1 reads elements with Get, so a HOLE iterates as `undefined`
//    rather than being skipped.
// 6. 23.1.5.2.2: %ArrayIteratorPrototype%'s @@toStringTag is
//    "Array Iterator".
const a = ['x', 'y'];
console.log(typeof a.values, typeof a.keys, typeof a.entries);
console.log(a[Symbol.iterator] === a.values);
console.log(a.values === Array.prototype.values);
console.log(Array.prototype[Symbol.iterator] === Array.prototype.values);
const it = a.values();
const first = it.next();
console.log(first.value, first.done);
console.log(it.next().value);
console.log(it.next().done);
console.log(it.next().done);
const ks = [];
for (const k of a.keys()) ks.push(k);
console.log(ks.join(','));
const parts = [];
for (const [i, v] of a.entries()) parts.push(i + ':' + v);
console.log(parts.join(','));
const it2 = a.values();
console.log(it2[Symbol.iterator]() === it2);
const h = [1, 2, 3];
delete h[1];
const seen = [];
for (const v of h.values()) seen.push(typeof v);
console.log(seen.join(','));
console.log(Object.prototype.toString.call(a.values()));
