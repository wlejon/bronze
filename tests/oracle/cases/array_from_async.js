// BLOCKED: `Hard runtime error: unsupported: Array.fromAsync is not
// implemented`.
//
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
// bronze refuses it BY NAME rather than shipping the synchronous half, and the
// reason is structural rather than a matter of effort: an `await` in bronze is a
// compiled state machine — `bronze_async_machine` splits a function at each
// suspension point and the runtime resumes it through a saved frame — and a
// NATIVE builtin has no such machine. A native `fromAsync` would have to be
// written as a hand-rolled promise chain that re-enters itself once per element,
// carrying the accumulated array, the iterator record and the index across every
// tick, with `IteratorClose` reachable from each of them; and every one of the
// four staleness bugs the moving collector has produced would be reachable from
// a state object living across a job-queue turn. The honest thing to ship first
// is the refusal.
//
// The expectation below is what the member owes when it lands.
async function main() {
  console.log((await Array.fromAsync([1, 2, 3])).join(','));
  console.log((await Array.fromAsync([Promise.resolve(1), 2])).join(','));
  console.log((await Array.fromAsync([1, 2], async (x) => x * 2)).join(','));
  console.log((await Array.fromAsync({ length: 2, 0: 'a', 1: 'b' })).join(','));

  async function* source() {
    yield 1;
    yield 2;
  }
  console.log((await Array.fromAsync(source())).join(','));
  console.log(Array.isArray(await Array.fromAsync([])));
  console.log((await Array.fromAsync([])).length);

  const p = Array.fromAsync([1]);
  console.log(typeof p.then);
  await p;

  try {
    await Array.fromAsync(7);
    console.log('no throw');
  } catch (e) {
    console.log(e.name);
  }
}
main();
