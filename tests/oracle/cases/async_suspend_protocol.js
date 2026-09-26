// Async functions and async generators suspended at await: a rejection
// thrown back in at the await is caught by the try around it, finally blocks
// run across awaits, for-await drives async generators and sync iterables of
// promises, deep async recursion resumes every frame, and the microtask order
// of the awaits is the one ECMA-262 gives.

async function fail(msg) {
  await null;
  throw new Error(msg);
}

async function catchAcross() {
  try {
    await fail("first");
  } catch (e) {
    console.log("caught across await:", e.message);
  }
  try {
    await Promise.reject("plain reason");
  } catch (e) {
    console.log("caught rejection:", e);
  } finally {
    await null;
    console.log("finally after await");
  }
  return "catchAcross done";
}

async function uncaught() {
  const x = await 1;
  throw new TypeError("escaped " + x);
}

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

async function forAwait() {
  let sum = 0;
  for await (const v of counter(4)) sum += v;
  console.log("for-await sum:", sum);
  const seen = [];
  for await (const v of [Promise.resolve("a"), "b", Promise.resolve("c")]) seen.push(v);
  console.log("for-await sync iterable:", seen.join(""));
}

async function depth(n) {
  if (n === 0) return 0;
  const below = await depth(n - 1);
  return below + 1;
}

async function asyncGenThrow() {
  async function* g() {
    try {
      yield 1;
      yield 2;
    } catch (e) {
      console.log("gen caught:", e);
      yield "recovered";
    }
  }
  const it = g();
  console.log(JSON.stringify(await it.next()));
  console.log(JSON.stringify(await it.throw("x")));
  console.log(JSON.stringify(await it.next()));
}

async function main() {
  console.log(await catchAcross());
  try {
    await uncaught();
  } catch (e) {
    console.log(e.name, e.message);
  }
  await forAwait();
  console.log("depth:", await depth(800));
  // Past the stack: the innermost call's RangeError rejects every level.
  try {
    await depth(100000);
  } catch (e) {
    console.log("too deep:", e.name);
  }
  await asyncGenThrow();
}

// The order of settled awaits against plain promise jobs.
async function tickA() {
  console.log("A0");
  await null;
  console.log("A1");
  await null;
  console.log("A2");
}
async function tickB() {
  console.log("B0");
  await Promise.resolve();
  console.log("B1");
}
tickA();
tickB();
Promise.resolve().then(() => console.log("P1")).then(() => console.log("P2"));
console.log("sync end");

main().then(() => console.log("main done"));
