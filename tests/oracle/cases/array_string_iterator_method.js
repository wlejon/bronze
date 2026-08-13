// The language-visible iterator METHODS of an array and a string — the half of
// iteration that is a value a program can hold, next to the cursor-driven
// for-of the runtime always had.
//
// The shape below is how real code asks whether a value is iterable:
// `if (x[Symbol.iterator])` is a feature test, and the test, the method and
// the loop must be one story. What is pinned, from ECMA-262:
//
// 1. 23.1.3.41: `Array.prototype[Symbol.iterator]` is the SAME function
//    object as `Array.prototype.values` — identity, not a twin, which is why
//    `===` holds between the read off the array and the read off the
//    prototype object.
// 2. 23.1.5.1 CreateArrayIterator, stepped by 23.1.5.2.1: the values in index
//    order, then `{ value: undefined, done: true }`.
// 3. 27.1.2.1: `%IteratorPrototype%[Symbol.iterator]` returns `this`, which
//    an array iterator inherits — so an iterator is its own iterable, and the
//    hand-driven object composes with for-of and spread.
// 4. 22.1.3.36: a string's iterator lives on `String.prototype` and steps by
//    CODE POINT, so "hi" yields "h" — the same walk the for-of cursor takes.
//
// `cases/array_iterator_methods` and `cases/string_iterator_method` carry the
// kinds, the holes and the surrogate pairs; this case is the feature test
// itself and the identities it relies on.

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
