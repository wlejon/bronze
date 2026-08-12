// A typed array is iterable: 23.2.3.34 makes `%TypedArray%.prototype
// [Symbol.iterator]` the same function as `values`, and every construct that
// walks a value reaches it — `for-of` (14.7.5), spread (13.2.4.1), array
// destructuring (13.15.5.5) and a rest element.
//
// bronze's for-of does NOT build an iterator object for one: `rtOpenIterator`
// recognises a typed array and steps a cursor. The point of this case is that
// the fast path and the object a program can pull out by hand produce exactly
// the same values, in the same order.
const v = new Int16Array([10, 20, 30]);

let total = 0;
for (const x of v) total = total + x;
console.log(total);

console.log([...v]);
console.log([0, ...v, 99]);

const [first, second] = v;
console.log(first, second);

const [head, ...rest] = v;
console.log(head, rest);

// The iterator object itself, driven by hand.
const it = v[Symbol.iterator]();
console.log(it.next().value, it.next().value, it.next().value);
const done = it.next();
console.log(done.value, done.done);

// An iterator is its own iterable (27.1.2.1), so a partly consumed one can be
// handed to a for-of and resumes where it stopped.
const partial = v[Symbol.iterator]();
partial.next();
let tail = "";
for (const x of partial) tail = tail + x + ",";
console.log(tail);

// Iterating an empty view visits nothing rather than failing.
let count = 0;
for (const x of new Float32Array(0)) count = count + 1;
console.log(count, [...new Float32Array(0)].length);

// A typed array built FROM an iterable, which is 23.2.5.1 step 5.b.ii taking
// the iterator branch rather than the array-like one.
const fromSet = new Uint8Array(new Set([3, 1, 3, 2]));
console.log(fromSet);
const fromView = new Float32Array(new Int8Array([1, 2, 3]));
console.log(fromView);

// The element the loop sees is a plain number, so ordinary arithmetic and
// `typeof` apply to it.
for (const x of new Uint8Array([7])) console.log(typeof x, x + 1);
