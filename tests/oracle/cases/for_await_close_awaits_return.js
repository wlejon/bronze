// BLOCKED: leaving a for-await loop early does not await the iterator's
// `return()` result.
//
// AsyncIteratorClose (7.4.13) calls `return()` and, for a normal completion
// such as `break`, awaits what it returns before the loop is left: an async
// generator's finally blocks run before the statement after the loop, and a
// hand-written iterator's `return()` promise settles first. bronze calls
// `return()` (async_iter.close) and moves on without the await, so the
// statement after the loop runs one or more ticks early.

async function* counter(n) {
  try {
    for (let i = 0; i < n; i++) {
      await null;
      yield i;
    }
  } finally {
    console.log("counter finally");
  }
}

const slowClose = {
  [Symbol.asyncIterator]() {
    let i = 0;
    return {
      next() {
        return Promise.resolve({ value: i++, done: false });
      },
      return() {
        console.log("return called");
        return Promise.resolve()
          .then(() => null)
          .then(() => {
            console.log("return settled");
            return { done: true };
          });
      },
    };
  },
};

async function main() {
  for await (const v of counter(10)) {
    if (v === 2) break;
  }
  console.log("after break 1");
  for await (const v of slowClose) {
    if (v === 1) break;
  }
  console.log("after break 2");
}
main();
