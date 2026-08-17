// BLOCKED: `Hard runtime error: unsupported: Promise.try is not implemented`.
//
// `Promise.try(f, ...args)` (ES2025, ECMA-262 27.2.4.7) calls `f`
// SYNCHRONOUSLY and wraps whatever it does in a promise: a return value
// fulfils, a throw rejects, a returned promise is adopted. It exists because
// the idiom it replaces — `Promise.resolve().then(f)` — delays `f` by a tick,
// and `new Promise(r => r(f()))` reads as if a synchronous throw could escape.
//
// That synchronous shape is also why it is small work on top of what bronze
// already has: NewPromiseCapability over `this`, one Call, and the same
// resolve/reject pair the executor form already builds. It is refused by name
// today rather than half-built; `Promise.withResolvers`, its ES2024 neighbour,
// is implemented and pinned in cases/promise_with_resolvers.js.
//
// The expectation below is what the member owes when it lands, and every line
// of it is a spec consequence rather than an observation:
//
//   - `f` runs before `Promise.try` returns, so `order` holds `sync,after` by
//     the time the synchronous phase ends.
//   - Step 4 is `Completion(Call(...))`, so nothing `f` throws escapes — and a
//     non-callable first argument is the SAME path (Call throws TypeError, the
//     completion captures it), which is why `Promise.try(5)` rejects instead of
//     throwing.
//   - Adoption costs two extra ticks: resolving with a thenable queues a
//     NewPromiseResolveThenableJob, whose `then` call queues the reaction that
//     finally settles the outer promise. So `adopt` prints LAST, after the
//     `non-callable` line that was queued after it.
const order = [];

Promise.try(() => {
  order.push('sync');
  return 1;
}).then((v) => console.log('resolved', v));
order.push('after');
console.log('sync phase', order.join(','));

let escaped = 'none';
try {
  Promise.try(() => {
    throw new RangeError('boom');
  }).then(() => console.log('not reached'), (e) => console.log('rejected', e.name, e.message));
} catch (e) {
  escaped = e.name;
}
console.log('escaped', escaped);

// Trailing arguments are forwarded to `f`.
Promise.try((a, b) => a + b, 2, 3).then((v) => console.log('args', v));

// A returned promise is adopted: the handler sees the inner value.
Promise.try(() => Promise.resolve('adopted')).then((v) => console.log('adopt', v));

// Not callable: a rejection, not a throw.
Promise.try(5).then(() => console.log('not reached'), (e) => console.log('non-callable', e.name));

// The return value is always a promise.
console.log('thenable', typeof Promise.try(() => 0).then);
