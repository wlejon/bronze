// The TERMINAL iterator helpers (ECMA-262 27.1.4.1): `reduce`, `toArray`,
// `forEach`, `some`, `every` and `find` — the six that consume an iterator and
// answer a value rather than another iterator.
//
// Derived from ECMA-262:
//
// 1. 27.1.4.1.10 `reduce`: with no initial value the FIRST element becomes the
//    accumulator and the reducer's counter starts at 1; with one, the counter
//    starts at 0. `reduce(f, undefined)` PASSED an initial value — the test is
//    "is the argument present", not "is it undefined" (step 5).
// 2. Step 5.b: `reduce` over an EMPTY iterator with no initial value is a
//    TypeError.
// 3. 27.1.4.1.13 `toArray` yields a real Array (a fresh one each call), and
//    `Array.isArray` of it is true.
// 4. 27.1.4.1.9 `forEach` answers `undefined` and visits every element, with
//    the same (value, counter) callback shape as `map`.
// 5. 27.1.4.1.12 `some`, 27.1.4.1.6 `every` and 27.1.4.1.8 `find` SHORT-CIRCUIT
//    and then CLOSE the iterator (each spells `IteratorClose(iterated,
//    NormalCompletion(...))`). So the underlying generator's `finally` runs at
//    the deciding element, and the iterator is left exhausted — a second walk
//    of the same iterator yields nothing.
// 6. The empty-iterator answers are the vacuous ones: `some` is false, `every`
//    is true, `find` is undefined.
// 7. All six read `next` off the receiver (7.4.1 GetIteratorDirect), so a bare
//    `{next}` object works and no brand check stands in the way.
function* count(n) {
  for (let i = 1; i <= n; i += 1) yield i;
}

// 1: reduce, both arms, and the counter each sees.
console.log(count(4).reduce((a, b) => a + b));
console.log(count(4).reduce((a, b) => a + b, 100));
console.log(count(3).reduce((a, b, i) => `${a}|${b}@${i}`));
console.log(count(3).reduce((a, b, i) => `${a}|${b}@${i}`, 'z'));
console.log(count(2).reduce((a, b) => a + b, undefined));

// 2: the empty-with-no-initial-value TypeError.
try {
  count(0).reduce((a, b) => a + b);
  console.log('no throw');
} catch (e) {
  console.log(e instanceof TypeError, e.name);
}
console.log(count(0).reduce((a, b) => a + b, 'init'));

// 3: toArray.
const arr = count(3).toArray();
console.log(Array.isArray(arr), arr.length, arr.join(','));
console.log(count(0).toArray().length);

// 4: forEach.
const seen = [];
console.log(count(3).forEach((x, i) => { seen.push(`${i}=${x}`); }));
console.log(seen.join(' '));

// 5 & 6: the three predicates, their answers, and their closing.
console.log(count(4).some((x) => x === 3), count(4).some((x) => x > 9));
console.log(count(4).every((x) => x > 0), count(4).every((x) => x < 3));
console.log(count(4).find((x) => x % 2 === 0), count(4).find((x) => x > 9));
console.log(count(0).some(() => true), count(0).every(() => false), count(0).find(() => true));

// The closing is observable through a generator's `finally`.
function* logged(tag) {
  try {
    yield 1;
    yield 2;
    yield 3;
  } finally {
    console.log(`closed ${tag}`);
  }
}
console.log(logged('some').some((x) => x === 2));
console.log(logged('every').every((x) => x === 1));
console.log(logged('find').find((x) => x === 1));

// And the iterator really is left exhausted.
const shared = count(4);
console.log(shared.some((x) => x === 2));
console.log(shared.toArray().length);

// The counter `some` passes, to pin that it counts from 0.
const indices = [];
count(3).some((x, i) => { indices.push(i); return x === 3; });
console.log(indices.join(','));

// 7: a bare protocol object, with no @@iterator and no prototype chain to
// %Iterator.prototype% — reached through `Iterator.from`.
let k = 0;
const bare = { next() { k += 1; return k <= 3 ? { done: false, value: k } : { done: true }; } };
console.log(Iterator.from(bare).reduce((a, b) => a * b, 2));

// A callback that throws propagates ITS error, and closes the iterator first.
try {
  logged('throwing').forEach(() => { throw new RangeError('stop'); });
} catch (e) {
  console.log(e.name, e.message);
}
