// Stage R3, observability class (a) again, on the other side of the boundary:
// a call the static plan cannot follow.
//
// `hook` arrives as a parameter, is called through a property of an object the
// module reassigns, and ends up calling back into a closure over the same
// record. Nothing in `run`'s text enumerates that, so the region must end at
// the call — and it must end at it even though the call looks harmless.
//
// If it did not, `n` would count 1, 2, 3, 4 in a register while the heap slot
// said 100, and both lines below would change.
function make(hook) {
  let n = 0;

  function run(iters) {
    let out = 0;
    for (let i = 0; i < iters; i++) {
      n = n + 1;
      hook();
      out = out + n;
    }
    return out;
  }

  const reset = function () {
    n = 100;
  };

  return { run: run, reset: reset, get: function () { return n; } };
}

const box = {};
const m = make(function () {
  box.f();
});
box.f = function () {
  m.reset();
};

console.log(m.run(4));
console.log(m.get());
