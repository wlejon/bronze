// The six statics of 27.2.4, each waited for before the next one starts, so
// that what is pinned is each combinator's ANSWER and not the interleaving of
// six independent chains (promise_ordering pins the interleaving).
//
// Every line of the expectation was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.2.4.1 `Promise.all`: resolves with an array of the values IN INPUT
//   ORDER, whatever order the elements settled in, and rejects with the first
//   rejection reason. Each element goes through PromiseResolve (27.2.4.7), so
//   a non-promise element is simply its own value.
// * 27.2.4.2 `Promise.allSettled`: never rejects. Each result is an object
//   with `status` "fulfilled" plus `value`, or "rejected" plus `reason`, in
//   input order.
// * 27.2.4.5 `Promise.race`: settles with the first element to settle. An
//   element that is already settled beats a promise that never settles, since
//   only the already-settled one ever queues a job.
// * 27.2.4.3 `Promise.any`: resolves with the first FULFILLED value, ignoring
//   rejections; if every element rejects it rejects with an AggregateError
//   whose `errors` property holds the reasons in input order.
// * 20.5.7.3.2: `AggregateError.prototype.name` is "AggregateError".
//
// NOT pinned here: the AggregateError's `message`. 27.2.4.3 says only that a
// newly created AggregateError object is the reason — it fixes no text — so a
// message is an implementation's choice and an oracle expectation is not the
// place to freeze one.

Promise.all([1, Promise.resolve(2), 3]).then(function (values) {
  console.log('all: ' + values.join(','));
  return Promise.all([Promise.resolve(1), Promise.reject('boom'), Promise.resolve(3)]);
}).then(function () {
  console.log('unreachable');
}, function (reason) {
  console.log('all rejected: ' + reason);
  return Promise.allSettled([Promise.resolve('a'), Promise.reject('b')]);
}).then(function (results) {
  console.log('settled length: ' + results.length);
  console.log(results[0].status + ' ' + results[0].value);
  console.log(results[1].status + ' ' + results[1].reason);
  return Promise.race([new Promise(function () {}), Promise.resolve('fast')]);
}).then(function (winner) {
  console.log('race: ' + winner);
  return Promise.any([Promise.reject('no1'), Promise.resolve('yes'), Promise.reject('no2')]);
}).then(function (first) {
  console.log('any: ' + first);
  return Promise.any([Promise.reject('x'), Promise.reject('y')]);
}).then(function () {
  console.log('unreachable');
}, function (err) {
  console.log('any rejected: ' + err.name);
  console.log('errors: ' + err.errors.join(','));
  console.log('is AggregateError: ' + (err instanceof AggregateError));
  console.log('is Error: ' + (err instanceof Error));
});

console.log('sync end');
