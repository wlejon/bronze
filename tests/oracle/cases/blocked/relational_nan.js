// BLOCKED, and it is a SILENT WRONG ANSWER in the NUMERIC fast path: `NaN <= 1`
// is `true`.
//
// lower_expr_binary.cpp desugars `a <= b` to `!(a > b)` and `a >= b` to
// `!(a < b)`. That identity holds over a total order and NaN does not give one.
// ECMA-262 13.10 does not use it: `a <= b` evaluates IsLessThan(b, a), and
// returns false when the result is true OR **undefined** — undefined being what
// 13.10.1 step 4.c returns when either operand is NaN. bronze's `!` maps that
// same undefined to true, so every `<=` and `>=` involving a NaN is a confident
// yes where the language says no.
//
// This is a sibling of `blocked/string_relational`, and they share a file and
// would likely share an edit, but they are not the same bug and neither one
// implies the other. That case is about a branch never taken — two strings are
// compared as numbers because step 3 is missing. This one is about a rewrite
// that is invalid on its own terms, and it is wrong for `f64` operands that
// inference proved, with no dynamic value and no string anywhere near it.
//
// That is what makes it the more serious of the two. `dynamic` is the fallback
// and the native path is the reason the project exists, so a wrong answer that
// needs a proof to reach is worse than one that needs a string. The four
// operators are also not equally broken: `<` and `>` are correct, because
// CmpLt and CmpGt on f64 already answer false for NaN, exactly as 13.10.1
// step 4.c requires. Only the two spelled as negations are wrong, which is why
// this case pins all four next to each other.
//
// It survived because `cases/binary_math` compares numbers that are never NaN,
// and `cases/nan_and_infinity` pins NaN's arithmetic and its `===` but never
// orders it. Neither would move if this were fixed or broken again.
//
// What this pins when it lands, from ECMA-262 13.10 (the four operators) and
// 13.10.1 IsLessThan step 4.c (either operand NaN produces undefined):
//
// 1. All four relational operators are false when either side is NaN, which
//    includes NaN against itself. `<` and `>` already are; `<=` and `>=` are
//    the regression this case exists to hold.
// 2. `<=` and `>=` still answer correctly for ordinary numbers, including the
//    equal case that is the whole reason the operator differs from `<`.
// 3. It is wrong inside a function whose parameters inference proves are
//    numbers, so the native path is the one being pinned and not a boxed one.
// 4. A bounds filter is the shape a real program loses to: `x <= 2` currently
//    admits NaN, so one bad element passes a range check silently.
//
// When it passes, promote it and rewrite this header to say what it pins.

const n = NaN;
console.log(n <= 1, n >= 1, n < 1, n > 1);
console.log(n <= n, n >= n);
console.log(0 / 0 <= 5, 0 / 0 >= 5);

console.log(1 <= 2, 2 <= 2, 3 <= 2);
console.log(1 >= 2, 2 >= 2, 3 >= 2);

function atMost(a, b) {
  return a <= b;
}
console.log(atMost(NaN, 1), atMost(1, 2), atMost(2, 1));

const xs = [3, NaN, 1];
console.log(xs.filter(function (x) { return x <= 2; }).length);
