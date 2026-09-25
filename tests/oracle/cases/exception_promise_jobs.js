// A throw inside a promise job becomes the rejection of that job's derived
// promise (ECMA-262 27.2.2.1 NewPromiseReactionJob: an abrupt handler result
// rejects the capability), and a throw after an `await` rejects the async
// function's promise (27.7.5.1). Neither ends the program: the job queue keeps
// draining. A rejection nothing handles is reported on stderr, which this case
// does not pin; stdout shows the program carrying on around it.

console.log("start");

Promise.resolve(1)
  .then(function (v) {
    throw new Error("in then " + v);
  })
  .then(function () {
    console.log("skipped");
  })
  .catch(function (e) {
    console.log("caught: " + e.message);
    return "recovered";
  })
  .then(function (v) {
    console.log("after catch: " + v);
  });

// A throw in a catch handler rejects the next promise in the chain.
Promise.reject("first")
  .catch(function (r) {
    throw "second from " + r;
  })
  .catch(function (r) {
    console.log("caught: " + r);
  });

// A throw from a callee called inside the handler.
function helper() {
  throw new TypeError("helper");
}
Promise.resolve()
  .then(function () {
    helper();
  })
  .then(null, function (e) {
    console.log("caught: " + e.name + " " + e.message);
  });

// async: throw before and after an await.
async function beforeAwait() {
  throw "before await";
}
async function afterAwait() {
  await null;
  await null;
  throw "after await";
}
beforeAwait().catch(function (r) { console.log("caught: " + r); });
afterAwait().catch(function (r) { console.log("caught: " + r); });

// try/catch in an async function around an await of a rejected promise.
async function catchesAwait() {
  try {
    await Promise.reject("awaited rejection");
  } catch (r) {
    return "handled " + r;
  } finally {
    console.log("async finally");
  }
}
catchesAwait().then(function (v) { console.log(v); });

// A throw from a job nobody handles: its derived promise is rejected and
// reported on stderr. The queue keeps going.
Promise.resolve().then(function () {
  throw new Error("unhandled in job");
});

// finally's handler throwing replaces the settled value.
Promise.resolve("fine")
  .finally(function () {
    throw "from finally";
  })
  .catch(function (r) {
    console.log("caught: " + r);
  });

Promise.resolve().then(function () {
  console.log("microtask still runs");
});

Promise.resolve().then(function () {
  return Promise.resolve().then(function () {
    return Promise.resolve().then(function () {
      console.log("late job ran");
    });
  });
});

console.log("end");
