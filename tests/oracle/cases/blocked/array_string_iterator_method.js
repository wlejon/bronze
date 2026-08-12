// BLOCKED: `unsupported: Array.prototype[Symbol.iterator] is not implemented
// (neither is Array.prototype.values, which 23.1.3.34 makes the same function
// object)`.
//
// This refusal is NEW, and it arrived with the well-known symbol. The hook used
// to be the string `"@@iterator"`, which no array or string ever carried, so
// `a[Symbol.iterator]` read as `undefined` — the silent fallback the house
// rules forbid, saying arrays are not iterable while `for (const x of a)` was
// walking one. Making the key a real symbol put the read on the member path,
// where a name ECMA-262 defines and bronze has not built is diagnosed by name.
//
// What bronze HAS is a cursor: `for-of`, spread, destructuring and rest step an
// array and a string directly, and never build an object to do it. That is why
// the language-visible method is missing while the language-visible ITERATION
// works, and it is the whole of the gap this case pins.
//
// The reason it is worth a case rather than a note: the shape below is how real
// code asks whether a value is iterable. `if (x[Symbol.iterator])` is a feature
// test, and a feature test that aborts the process is worse for a library than
// one that answers. three.js r160 does not write it — the milestone passes —
// but it is ordinary JavaScript and bronze does not compile it.
//
// The expectation is derived from ECMA-262, not from what bronze prints:
//   - 23.1.3.41: `Array.prototype[Symbol.iterator]` is the SAME function object
//     as `Array.prototype.values`, which is why `===` holds and why the two
//     land together rather than one at a time.
//   - 23.1.5.1 CreateArrayIterator, stepped by 23.1.5.2.1: the values in index
//     order, then `{ value: undefined, done: true }`.
//   - 27.1.2.1: `%IteratorPrototype%[Symbol.iterator]` returns `this`, which an
//     array iterator inherits — so an iterator is its own iterable.
//   - 22.1.3.36: a string's iterator steps by CODE POINT, so "hi" yields "h".
//
// When it passes, promote it and rewrite this header to say what it pins.

const a = [10, 20];
console.log(typeof a[Symbol.iterator]);
console.log(a[Symbol.iterator] === Array.prototype.values);

const it = a[Symbol.iterator]();
console.log(it.next().value, it.next().value, it.next().done);
console.log(it[Symbol.iterator]() === it);

if (a[Symbol.iterator]) console.log('detected');

const s = 'hi';
console.log(typeof s[Symbol.iterator]);
console.log(s[Symbol.iterator]().next().value);
