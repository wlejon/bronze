// THENABLE ADOPTION: what happens when a promise is resolved with an object
// that has a callable `then`.
//
// Every line of the expectation was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.2.1.3.2 steps 9-10: resolving with an object READS `then` off it. The
//   read is an ordinary Get, so a getter would run here and its throw would
//   become the rejection.
// * 27.2.1.3.2 steps 11-12: an object whose `then` is not callable is NOT a
//   thenable. It fulfills the promise as an ordinary value — so a `{ then: 42 }`
//   comes back out of the chain unchanged, `then` property and all.
// * 27.2.1.3.2 step 13 and 27.2.2.2: a real thenable's `then` is called from a
//   JOB, not from the resolve that adopted it. That is what makes adoption
//   cost a tick, and it is why nothing a thenable does can be observed inside
//   the `Promise.resolve` call that adopted it.
// * 27.2.2.2 step 3: the job calls `then` with a FRESH resolving-function
//   pair, and its throw rejects the promise — through that pair's own
//   [[AlreadyResolved]] latch, so a `then` that resolved and then threw keeps
//   its first answer.
// * 27.2.4.7 step 2 is NOT reached for any of these: none of them is an
//   intrinsic promise, so each is wrapped and the wrapper is what adopts.

console.log('start');

const thenable = {
  then: function (resolve) {
    console.log('thenable.then ran');
    resolve('adopted');
  }
};

Promise.resolve(thenable).then(function (v) {
  console.log('adopted value: ' + v);
  const throwing = {
    then: function () { throw 'thrown by then'; }
  };
  return Promise.resolve(throwing);
}).then(function () {
  console.log('unreachable');
}, function (reason) {
  console.log('rejected with: ' + reason);
  const notThenable = { then: 42 };
  return Promise.resolve(notThenable);
}).then(function (v) {
  console.log('non-callable then survives: ' + v.then);
  const settledTwice = {
    then: function (resolve, reject) {
      resolve('first');
      reject('second');
      resolve('third');
    }
  };
  return Promise.resolve(settledTwice);
}).then(function (v) {
  console.log('first settle wins: ' + v);
});

console.log('end');
