// `Promise.withResolvers` (ECMA-262 27.2.4.8) — one promise capability handed
// out as an object instead of being hidden inside an executor.
//
// Derived from ECMA-262:
//
// 1. Steps 4-6 are CreateDataPropertyOrThrow, so the three properties are
//    ORDINARY data properties: enumerable, and in the order `promise`,
//    `resolve`, `reject`. That makes `Object.keys` of the result exactly those
//    three names, which is what distinguishes it from an intrinsic's own
//    members (they are all non-enumerable).
// 2. The `resolve` and `reject` it hands over are the pair 27.2.1.3 builds for
//    the promise, so they behave exactly as the executor's do: the LATCH is
//    shared, and the first settle wins — a `resolve` after a `reject`, or a
//    second `resolve`, changes nothing.
// 3. The promise is an ordinary intrinsic promise: `then`, `catch` and the
//    combinators all work on it, and it resolves through the same job queue, so
//    every handler runs after the synchronous part of the program.
// 4. Resolving with a THENABLE adopts its state (27.2.1.3.2 step 9), which is
//    the one behaviour a plain "store the value" implementation would get wrong.
// 5. The `resolve` function is detachable — it does not read `this` — which is
//    the entire point of the member.
// 6. Step 2 is NewPromiseCapability(C), which requires a constructor: a
//    detached `withResolvers` called with no receiver is a TypeError.
const first = Promise.withResolvers();
console.log(typeof first.promise, typeof first.resolve, typeof first.reject);
console.log(Object.keys(first).join(','));
console.log(first.resolve === first.reject);
console.log(typeof first.promise.then, typeof first.promise.catch);

// 3 & 5: resolve from outside the closure, through a detached reference.
const resolveIt = first.resolve;
resolveIt('one');
first.promise.then((v) => console.log('a', v));

// 2: the latch. A second settle of either kind is ignored.
const second = Promise.withResolvers();
second.resolve('kept');
second.resolve('ignored');
second.reject(new Error('ignored too'));
second.promise.then((v) => console.log('b', v), () => console.log('b rejected'));

const third = Promise.withResolvers();
third.reject(new RangeError('boom'));
third.resolve('too late');
third.promise.then(() => console.log('c resolved'), (e) => console.log('c', e.name, e.message));

// 4: resolving with a thenable adopts it.
const fourth = Promise.withResolvers();
fourth.resolve({ then(onFulfilled) { onFulfilled('adopted'); } });
fourth.promise.then((v) => console.log('d', v));

// A resolver called with no argument resolves with undefined.
const fifth = Promise.withResolvers();
fifth.resolve();
fifth.promise.then((v) => console.log('e', v));

// The promise takes part in the combinators like any other.
const sixth = Promise.withResolvers();
const seventh = Promise.withResolvers();
Promise.all([sixth.promise, seventh.promise, 3]).then((vs) => console.log('f', vs.join(',')));
sixth.resolve(1);
seventh.resolve(2);

// Each call is a fresh capability.
const again = Promise.withResolvers();
console.log(again.promise === first.promise, again.resolve === first.resolve);

// 6: the receiver must be a constructor.
const detached = Promise.withResolvers;
try {
  detached();
  console.log('no throw');
} catch (e) {
  console.log(e.name);
}

// 3: nothing above has run its handler yet — the queue drains after this line.
console.log('sync done');
