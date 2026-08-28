// A guard that holds for a while and then fails: the exit from a guarded
// numeric region happens IN THE MIDDLE of a loop, and what crosses the
// trampoline is the sum accumulated so far, re-boxed
// (src/lower/guard_region.h, the resume invariant).
//
// The exact tail of the string is the whole point. `15` is five iterations of
// +1+2 done as an `fadd` on a value that never went near a box; everything
// after it is the original operator, and the digits could only be there if the
// partial sum arrived across the trampoline intact and in the right place.
//
// Control leaves the fast copy at most once per entry and never returns, so a
// property that goes back to being a Number does NOT put the loop back on the
// fast path — the trailing `9`s below are the proof.

function run(o, flipAt, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    if (i === flipAt) {
      o.a = "s";
    }
    if (i === flipAt + 2) {
      o.a = 9;
    }
    total = total + o.a + o.b;
  }
  return total;
}

console.log(run({ a: 1, b: 2 }, 5, 9));

// The same shape with the flip on the FIRST iteration: nothing is accumulated
// on the fast path at all.
console.log(run({ a: 1, b: 2 }, 0, 4));

// And with a flip past the end, so the fast path runs the whole loop.
console.log(run({ a: 1, b: 2 }, 100, 6));

// The second operand failing instead of the first: the add before it has
// already run.
function runB(o, flipAt, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    if (i === flipAt) {
      o.b = "t";
    }
    total = total + o.a + o.b;
  }
  return total;
}
console.log(runB({ a: 1, b: 2 }, 4, 6));
