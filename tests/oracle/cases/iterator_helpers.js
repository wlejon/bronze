// The LAZY iterator helpers (ECMA-262 27.1.4.1): `map`, `filter`, `take`,
// `drop` and `flatMap`, plus `Iterator` and `Iterator.from` (27.1.3).
//
// Derived from ECMA-262:
//
// 1. 27.1.4.1 puts all eleven helpers on %Iterator.prototype%, which every
//    built-in iterator inherits (27.1.2 is the parent of %GeneratorPrototype%
//    27.5.1, %ArrayIteratorPrototype% 23.1.5.2, %MapIteratorPrototype%
//    24.1.5.2, %SetIteratorPrototype% 24.2.5.2 and %StringIteratorPrototype%
//    22.1.5.1) — so all five kinds answer all eleven.
// 2. 27.1.3.2 makes `Iterator.prototype` that SAME object, so
//    `Iterator.prototype.map === [].values().map` and `gen() instanceof
//    Iterator` is true.
// 3. Each of the five returns an Iterator Helper object (27.1.4.2) — one
//    prototype for all five, tagged "Iterator Helper" by 27.1.4.2.3, with no
//    own property of any kind because `next` and `return` live on that
//    prototype.
// 4. The callbacks are passed (value, counter) with the counter starting at 0
//    and counting the elements the HELPER saw: `filter`'s counter therefore
//    counts every element and not just the kept ones.
// 5. Laziness is observable: 27.1.4.1.5's generator body reads one element per
//    `next`, so `map(f).take(2)` over a five-element iterator calls `f` twice.
// 6. `take(n)` stops after n and `drop(n)` skips n; both take
//    ToIntegerOrInfinity of their argument (27.1.4.1.11 step 6, 27.1.4.1.4
//    step 6), so `take(1.9)` is `take(1)` and `take(Infinity)` is everything.
// 7. `flatMap` opens each mapped value with GetIteratorFlattenable
//    (27.1.4.1.7 step 6.d.i), which accepts an array, a generator, and a bare
//    `{next}` object.
// 8. `Iterator.from` (27.1.3.1.1) returns its argument UNCHANGED when that
//    argument already inherits %Iterator.prototype% (step 3), and wraps it
//    otherwise — so a bare `{next}` object gains the helpers.
function* count(n) {
  for (let i = 1; i <= n; i += 1) yield i;
}

// 1 & 2: where the helpers live.
console.log(typeof Iterator, typeof Iterator.from);
console.log(Iterator.prototype.map === [].values().map);
console.log(typeof count(1).map, typeof new Set().values().filter);
console.log(typeof new Map().entries().take, typeof ''[Symbol.iterator]().drop);
console.log(count(1) instanceof Iterator, [].values() instanceof Iterator);

// 3: the helper object.
const helper = count(3).map((x) => x);
console.log(Object.prototype.toString.call(helper));
console.log(Object.getOwnPropertyNames(helper).length);
console.log(Object.getPrototypeOf(helper) === Object.getPrototypeOf(count(1).filter((x) => x)));
console.log(typeof helper.next, typeof helper.return);
console.log(helper[Symbol.iterator]() === helper);

// map / filter, and the counter each callback is passed.
console.log(count(4).map((x) => x * 10).toArray().join(','));
console.log(count(3).map((x, i) => `${i}:${x}`).toArray().join(' '));
console.log(count(5).filter((x) => x % 2 === 1).toArray().join(','));
console.log(count(4).filter((x, i) => i > 1).toArray().join(','));

// 5: laziness. `calls` counts what the mapper actually ran on.
let calls = 0;
const lazy = count(5).map((x) => {
  calls += 1;
  return x;
});
console.log(calls);
console.log(lazy.take(2).toArray().join(','), calls);

// 6: take and drop.
console.log(count(5).take(2).toArray().join(','));
console.log(count(5).take(0).toArray().length);
console.log(count(3).take(9).toArray().join(','));
console.log(count(5).take(1.9).toArray().join(','));
console.log(count(5).take(Infinity).toArray().join(','));
console.log(count(5).drop(3).toArray().join(','));
console.log(count(5).drop(0).toArray().join(','));
console.log(count(3).drop(9).toArray().length);
console.log(count(6).drop(1).take(2).toArray().join(','));

// 7: flatMap over three different flattenable kinds.
console.log(count(3).flatMap((x) => [x, x * 10]).toArray().join(','));
console.log(count(2).flatMap((x) => count(x)).toArray().join(','));
console.log(count(2).flatMap(() => []).toArray().length);
console.log(
  count(2)
    .flatMap((x) => {
      let done = false;
      return { next() { const d = done; done = true; return { done: d, value: x }; } };
    })
    .toArray()
    .join(','),
);

// 8: Iterator.from.
const gen = count(2);
console.log(Iterator.from(gen) === gen);
console.log(Iterator.from([7, 8]).toArray().join(','));
console.log(Iterator.from('ab').toArray().join(','));
let n = 0;
const bare = { next() { n += 1; return n <= 2 ? { done: false, value: n * 3 } : { done: true }; } };
const wrapped = Iterator.from(bare);
console.log(wrapped === bare, wrapped instanceof Iterator);
console.log(wrapped.map((x) => x + 1).toArray().join(','));

// The five kinds all reach the helpers.
console.log([1, 2, 3].values().map((x) => x + 1).toArray().join(','));
console.log(new Set([1, 2, 3]).values().filter((x) => x > 1).toArray().join(','));
console.log(new Map([['a', 1], ['b', 2]]).entries().map(([k, v]) => k + v).toArray().join(','));
console.log('abc'[Symbol.iterator]().take(2).toArray().join(''));
console.log('a1b2'.matchAll(/\d/g).map((m) => m[0]).toArray().join(','));
