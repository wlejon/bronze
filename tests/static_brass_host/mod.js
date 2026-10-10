// The module of the static-brass-host test (CMakeLists.txt says what the host
// is and why). Every coroutine here starts with brass_coro_create, the call a
// generator or async function's stub makes in compiled code, and is then
// started, suspended and resumed by the shared runtime: the frame has to be
// one the runtime's coroutine registry knows.

function* count(n) {
  for (let i = 0; i < n; i++) yield i;
}
let sum = 0;
for (const v of count(5)) sum += v;
console.log("generator sum=" + sum);

// An async function with no await at all still starts a frame.
async function plain(x) { return x * 2; }

async function gather(xs) {
  const out = [];
  for (const x of xs) {
    const v = await x;
    out.push(v);
  }
  return out.join(",");
}

plain(21).then((v) => console.log("plain=" + v));
gather([1, Promise.resolve(2), new Promise((resolve) => resolve(3))]).then(
  (v) => console.log("gathered=" + v),
  (e) => console.log("gather threw " + e));

// Called by the host after the top level has finished: a frame started from a
// host call, not from the module's own entry.
globalThis.fromHost = function () {
  gather([4, Promise.resolve(5)]).then((v) => console.log("host gathered=" + v));
};
