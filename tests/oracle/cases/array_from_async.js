// `Array.fromAsync` (ECMA-262 23.1.2.1, ES2024) is `Array.from` over an ASYNC
// iterable, and it returns a promise for the array. The two differences from
// `Array.from` are the whole of the work:
//
//   - it opens the source with @@asyncIterator when there is one and falls back
//     to @@iterator otherwise, and it AWAITS every value it takes from either —
//     so `Array.fromAsync([Promise.resolve(1)])` is a promise for `[1]`, where
//     `Array.from` of the same input gives an array holding the promise;
//   - the mapper may be async, and each mapped value is awaited too, in order.
//
// An `await` in bronze is a compiled state machine, and a native builtin has
// none — so `arrayFromAsync` (builtin_constructors.cpp) is the machine written
// by hand: one state record with internal slots, one driver that runs the
// synchronous stretch between two awaits, and native closures over the record
// as the continuations. What this case pins is that the hand-written machine
// and the compiled one agree on every observable point: the values, the order,
// which failures are rejections rather than throws (3's AsyncFunctionStart
// makes every one of them a rejection, so `fromAsync(7)` — ToObject of a
// number, length 0 — resolves with `[]` and `fromAsync(null)` rejects), and
// that a mapper's throw closes the source before the rejection.
async function main() {
  console.log((await Array.fromAsync([1, 2, 3])).join(','));
  console.log((await Array.fromAsync([Promise.resolve(1), 2])).join(','));
  console.log((await Array.fromAsync([1, 2], async (x) => x * 2)).join(','));
  console.log((await Array.fromAsync({ length: 2, 0: 'a', 1: 'b' })).join(','));
  console.log((await Array.fromAsync({ length: 2, 0: Promise.resolve('p'), 1: 'q' })).join(','));

  async function* source() {
    yield 1;
    yield 2;
  }
  console.log((await Array.fromAsync(source())).join(','));
  console.log((await Array.fromAsync(source(), (x, i) => x * 10 + i)).join(','));
  console.log(Array.isArray(await Array.fromAsync([])));
  console.log((await Array.fromAsync([])).length);

  const p = Array.fromAsync([1]);
  console.log(typeof p.then);
  await p;

  // Every failure is a rejection, never a throw: the number is an array-like
  // of no elements, `null` has no @@asyncIterator to get, a non-function mapper
  // fails inside the closure, and a rejected element rejects the whole.
  try {
    console.log('number', (await Array.fromAsync(7)).length);
  } catch (e) {
    console.log('number threw', e.name);
  }
  let sync = 'none';
  try {
    Array.fromAsync(null).catch((e) => console.log('null', e.name));
  } catch (e) {
    sync = e.name;
  }
  console.log('escaped', sync);
  await Array.fromAsync([1], 5).catch((e) => console.log('mapper', e.name));
  await Array.fromAsync([1, Promise.reject(new RangeError('el'))])
      .catch((e) => console.log('element', e.name, e.message));

  // A mapper that throws closes the source (5.k.i.5.b.ii): the generator's
  // `return` runs, which is what the `finally` sees.
  let closed = false;
  async function* watched() {
    try {
      yield 1;
      yield 2;
    } finally {
      closed = true;
    }
  }
  await Array.fromAsync(watched(), (x) => { if (x === 2) throw new Error('map'); return x; })
      .catch((e) => console.log('closed', closed, e.message));
  // The same for a sync source: the iterator is closed by the wrapper.
  let syncClosed = false;
  const syncIter = {
    [Symbol.iterator]() {
      let i = 0;
      return {
        next() { i++; return { value: i, done: i > 3 }; },
        return() { syncClosed = true; return { done: true }; },
      };
    },
  };
  await Array.fromAsync(syncIter, (x) => { if (x === 2) throw new Error('smap'); return x; })
      .catch((e) => console.log('sync closed', syncClosed, e.message));

  // `thisArg` reaches the mapper, and the index is the second argument.
  const ctx = { base: 100 };
  console.log((await Array.fromAsync([1, 2], function (x, i) { return this.base + x + i; }, ctx)).join(','));

  // `this` is the constructor the result is built through (step 3.g / 3.j.i):
  // a subclass gets an instance of itself, filled through its own setters.
  class MyArr extends Array {}
  const mine = await MyArr.fromAsync([1, 2]);
  console.log(mine instanceof MyArr, mine.length, mine.join(','));

  // Order: `fromAsync` returns before any element is read past the first
  // await, and elements resolve in sequence, never concurrently.
  const order = [];
  const seq = Array.fromAsync([1, 2], async (x) => { order.push('map' + x); return x; });
  order.push('returned');
  await seq;
  console.log(order.join(','));
}
main();
