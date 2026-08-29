// GUARDED NUMERIC REGIONS and the GC ROOT SLOTS THE TWO COPIES SHARE.
//
// The pass duplicates a region into a fast copy carrying its numbers as `f64`
// and leaves the original blocks as the slow copy (src/lower/guard_region.h).
// The two are mutually exclusive, so `planFrame` lays their pinned `dynamic`
// values out FROM THE SAME BASE — one frame slot holding a fast value on one
// run and a slow value on another (codegen-llvm/llvm_frame.h).
//
// What that costs if it is wrong is a use-after-move, which is invisible until
// a collection happens between the store and the load. So every function here
// keeps SEVERAL dynamic values live across an ALLOCATING call on BOTH sides of
// a guard, and every case runs the fast copy first, allocates in it, then fails
// a guard and finishes in the slow copy holding different dynamic values across
// another allocation. Under `BRONZE_GC_STRESS=1` every one of those allocations
// is a collection, so a slot the overlay handed to two live values at once is a
// wrong answer here rather than a rare one.
//
// The arithmetic itself is 13.15.3 ApplyStringOrNumericBinaryOperator
// throughout: `+` over a String is concatenation, over anything else it is
// ToNumeric on both sides; `*` is ToNumeric unconditionally, so a numeric
// string multiplies and a non-numeric one is NaN.

// An object whose ToPrimitive ALLOCATES: 7.1.1 OrdinaryToPrimitive calls
// `valueOf` first for hint number, and this one builds thirty-three objects
// before it answers. It is what a slow copy runs while the values the fast copy
// handed it are live.
function heavy(answer) {
  return {
    valueOf() {
      const junk = [];
      for (let k = 0; k < 32; k++) junk.push({ k: k });
      return junk.length + answer;
    },
  };
}

// A LOOP region. `total` is the promoted accumulator; `it`, `tag` and `box` are
// dynamic, defined before the `skip` branch and read after it, so each keeps a
// pinned slot in whichever copy it is in — and `box` is an allocation standing
// between their definitions and their uses.
function tally(items) {
  let total = 0;
  let log = "";
  for (let i = 0; i < items.length; i++) {
    const it = items[i];
    const tag = it.tag;
    const box = { n: it.v, m: it.w };
    if (it.skip) {
      log = log + tag + "-";
      continue;
    }
    total = total + box.n + box.m;
    log = log + tag + ";";
  }
  return total + "|" + log;
}

function item(v, w, tag, skip) {
  return { v: v, w: w, tag: tag, skip: skip };
}

// Every iteration takes the fast copy: two guards hold, and the two allocations
// per iteration happen with `it`, `tag`, `box` and `log` live.
console.log(tally([item(1, 2, "a", false), item(3, 4, "b", false)]));
// The same, with an iteration that leaves through `continue` — the branch is
// what makes those three values outlive their defining block.
console.log(
  tally([item(1, 2, "a", false), item(9, 9, "s", true), item(3, 4, "b", false)]),
);
// The third iteration's `box.n` is a String, so the guard fails there: two
// iterations of the fast copy, then the rest of the loop in the slow copy,
// where `+` is concatenation from that point on.
console.log(
  tally([
    item(1, 2, "a", false),
    item(3, 4, "b", false),
    item("5", 6, "c", false),
    item(7, 8, "d", false),
  ]),
);
// The SECOND operand is what fails, one instruction later than the first.
console.log(tally([item(1, "2", "a", false), item(3, 4, "b", false)]));
// The value that fails the guard allocates while it is being coerced, with the
// slow copy's `it`, `tag`, `box` and `log` all live across it.
console.log(
  tally([
    item(1, 2, "a", false),
    item(heavy(0), 6, "c", false),
    item(7, 8, "d", false),
  ]),
);

// An ENTRY region: no loop, so the region is the whole function and the fast
// copy IS the entry. `pair` and `name` are dynamic and live across the string
// concatenation that builds `name` and across the arithmetic below it.
function combine(p, q) {
  const pair = { a: p.x, b: q.x };
  const name = p.tag + q.tag;
  const sum = pair.a * q.y + pair.b * p.y;
  return name + ":" + sum;
}

const left = { x: 2, y: 3, tag: "p" };
// Every guard holds: the fast copy runs end to end.
console.log(combine(left, { x: 4, y: 5, tag: "q" }));
// `q.x` is a String, so the entry guard fails and the whole body runs in the
// slow copy. 13.15.3 step 3 is ToNumeric on both operands for `*`, and 7.1.4
// ToNumber of "4" is 4, so the answer is the one the fast copy would have
// given.
console.log(combine(left, { x: "4", y: 5, tag: "q" }));
// A String that is not a number: 7.1.4.1 StringToNumber of "abc" is NaN, and
// NaN propagates through the sum.
console.log(combine(left, { x: "abc", y: 5, tag: "q" }));
// The failing value allocates while it is coerced.
console.log(combine(left, { x: heavy(0), y: 5, tag: "q" }));
