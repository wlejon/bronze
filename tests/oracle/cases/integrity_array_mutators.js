// `Array.prototype`'s mutators against the three integrity levels — the half of
// `cases/integrity_levels` that does not go through `a[i] = v`.
//
// Why it is a separate question. An element write is 13.15.2 PutValue, whose
// refusal is silent in sloppy code; every mutator below is defined with
// `Set(O, k, v, true)` or `DeletePropertyOrThrow` (ECMA-262 23.1.3), and that
// `true` makes a refusal a TypeError from the METHOD, whatever the strictness
// of the code that called it. So `a[0] = 9` on a frozen array does nothing and
// `a.fill(9)` on the same array throws, and the two are not inconsistent.
//
// Each method asks for exactly the capability its algorithm uses, which is why
// there are three answers and not one:
//
//  - `push` and `unshift` CREATE indices, so they need [[Extensible]] — and are
//    therefore refused by `preventExtensions` alone, without any help from
//    seal or freeze.
//  - `pop` and `shift` end in DeletePropertyOrThrow of the index that falls off
//    the end, so they need the elements to be configurable, which only `seal`
//    takes away. A merely non-extensible array pops and shifts perfectly well.
//  - `reverse` and `fill` only ever Set indices that are already there, so they
//    need writability, which only `freeze` takes away.
//
// The three "nothing to do" lines are not padding. Each of them is a call whose
// algorithm writes NOTHING for its arguments — `push()` with no arguments,
// `pop()` on an empty array, `reverse()` on a one-element array, `fill` over an
// empty range — and each therefore does not throw even on a frozen receiver.
// That is 10.4.2.4 ArraySetLength step 10 and 10.1.6.3 read literally: writing
// `length` to the value it already holds is accepted even when `length` is
// non-writable, because the descriptor validation compares values before it
// looks at the attribute. An implementation that guarded these methods with one
// blanket "is it frozen" test would throw for all four.
//
// `length` itself is only half observable here. It is non-configurable and
// writable from birth (10.4.2 ArrayCreate) and `freeze` is what makes it
// non-writable — but bronze refuses EVERY write to an array's `length` with a
// hard error, so the only place that attribute shows through is
// `Object.isFrozen`, which `cases/integrity_levels` pins. What is observable
// here is that the refused mutators leave `length` where it was.

function attempt(label, body) {
  try {
    console.log(label + ":", body());
  } catch (e) {
    console.log(label + ":", e instanceof TypeError, e.name);
  }
}

attempt("push frozen", function () {
  return Object.freeze([1, 2, 3]).push(4);
});
attempt("push sealed", function () {
  return Object.seal([1, 2, 3]).push(4);
});
attempt("push closed", function () {
  return Object.preventExtensions([1, 2, 3]).push(4);
});
attempt("push nothing", function () {
  return Object.freeze([1, 2, 3]).push();
});

attempt("pop closed", function () {
  return Object.preventExtensions([1, 2, 3]).pop();
});
attempt("pop sealed", function () {
  return Object.seal([1, 2, 3]).pop();
});
attempt("pop frozen", function () {
  return Object.freeze([1, 2, 3]).pop();
});
attempt("pop frozen empty", function () {
  return Object.freeze([]).pop();
});

attempt("shift closed", function () {
  return Object.preventExtensions([1, 2, 3]).shift();
});
attempt("shift sealed", function () {
  return Object.seal([1, 2, 3]).shift();
});

attempt("unshift closed", function () {
  return Object.preventExtensions([1, 2, 3]).unshift(0);
});
attempt("unshift nothing", function () {
  return Object.freeze([1, 2, 3]).unshift();
});

attempt("reverse frozen", function () {
  return Object.freeze([1, 2, 3]).reverse().join(",");
});
attempt("reverse sealed", function () {
  return Object.seal([1, 2, 3]).reverse().join(",");
});
attempt("reverse frozen one", function () {
  return Object.freeze([1]).reverse().join(",");
});

attempt("fill frozen", function () {
  return Object.freeze([1, 2, 3]).fill(0).join(",");
});
attempt("fill sealed", function () {
  return Object.seal([1, 2, 3]).fill(0).join(",");
});
attempt("fill empty range", function () {
  return Object.freeze([1, 2, 3]).fill(0, 1, 1).join(",");
});

// A refused mutator leaves the array exactly where it was — the length included.
const sealedArr = Object.seal([1, 2]);
try {
  sealedArr.push(3);
} catch (e) {
  console.log("push threw", e instanceof TypeError);
}
console.log(sealedArr.length, sealedArr.join(","));

const closedArr = Object.preventExtensions([1, 2, 3]);
console.log(closedArr.length);
closedArr.pop();
console.log(closedArr.length, closedArr.join(","));

// Reading is untouched, and every method that PRODUCES a new array produces an
// ordinary one: an integrity level is a fact about one object, not about the
// values it holds.
const frozen = Object.freeze([1, 2, 3]);
console.log(frozen.length, frozen.join(","), frozen.indexOf(2), frozen.includes(3));
const doubled = frozen.map(function (x) {
  return x * 2;
});
console.log(doubled.join(","), Object.isFrozen(doubled));
doubled.push(8);
console.log(doubled.join(","));
console.log(Object.keys(frozen).join(","));
