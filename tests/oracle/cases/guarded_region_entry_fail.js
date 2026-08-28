// The guard on the ENTRY EDGE of a guarded numeric region: a value defined
// outside the loop is tested once, in the block that jumps into it, and a
// failure goes to the original loop with the original values before a single
// iteration has run (src/lower/guard_region.h §"EntryGuard").
//
// What this pins is that the test is a BRANCH and not a coercion: an
// accumulator that is already a String must still concatenate, in source order,
// exactly as it would have without the pass — and the cost of finding that out
// must be paid once, not once per iteration, which is what the identical
// answers below say.

function build(o, n, seed) {
  let total = seed;
  for (let i = 0; i < n; i++) {
    total = total + o.a + o.b;
  }
  return total;
}

const o = { a: 1, b: 2 };

// The guard fails on the entry edge: `seed` is a String and every iteration
// concatenates.
console.log(build(o, 3, "x"));
// The same loop entered with a Number: the guard holds and the arithmetic runs.
console.log(build(o, 3, 0));
// Zero iterations: the guard is still asked, and its answer must not be
// observable in the result.
console.log(build(o, 0, "x"));
console.log(build(o, 0, 7));

// An object accumulator, which is neither a Number nor a String until
// ToPrimitive says so. 13.15.3 asks ToPrimitive with NO hint, so `valueOf` is
// tried before `toString`.
const box = { valueOf() { return 100; } };
console.log(build(o, 2, box));

// null and undefined seeds: ToNumber(null) is +0 and ToNumber(undefined) is
// NaN (7.1.4 table 14).
console.log(build(o, 2, null));
console.log(build(o, 2, undefined));

// A seed that is a String only for some calls, so one compiled function has to
// answer both ways.
function twice(seed) {
  return build(o, 1, seed) + "|" + build(o, 1, 10);
}
console.log(twice("s"));
console.log(twice(1));
