// The four relational operators against NaN, ECMA-262 13.10 and 13.10.1
// IsLessThan step 4.c.
//
// Every one of them is FALSE when either operand is NaN, NaN against itself
// included. Step 4.c produces *undefined* for a NaN operand, and 13.10 folds
// undefined to false for all four — which is why `<=` is not `!(a > b)` and
// `>=` is not `!(a < b)`: that identity needs a total order, and a negation
// maps the same undefined to true. `<` and `>` were always right here, because
// an ordered f64 compare answers false for NaN on its own; `cmp.le` and
// `cmp.ge` are what make the other two agree.
//
// What each line holds:
//
// 1. All four operators against NaN, and against NaN twice over, and against a
//    NaN that arrives from arithmetic (`0 / 0`) rather than from the literal.
// 2. `<=` and `>=` over ordinary numbers, the equal case included — the case
//    that is the whole reason these operators differ from `<` and `>`.
// 3. Inside a function whose parameters inference proves are numbers, so it is
//    the NATIVE compare being pinned and not a boxed one. `dynamic` is the
//    fallback and the proven path is the reason the project exists, so a wrong
//    answer reachable only through a proof is the worse kind.
// 4. A bounds filter, which is the shape a real program loses to: `x <= 2` must
//    not admit NaN, or one bad element passes a range check in silence.
//
// `cases/binary_math` walks the same operators over numbers that are never
// NaN, and `cases/nan_and_infinity` pins NaN's arithmetic and its `===` without
// ever ordering it. This is the case that orders it.

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
