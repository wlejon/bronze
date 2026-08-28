// A PARAMETER guarded on the function's own entry edge, which is what the
// entry region of `src/lower/guard_region.h` is: `h = b0`, `R` = every block,
// and the guard for a candidate defined by a parameter goes at the top of the
// fast copy with a trampoline into the original `b0` that carries nothing —
// nothing has been computed yet.
//
// The whole point of the guard being a BRANCH and not a coercion is that the
// program cannot tell it happened. So the same function is called with a
// Number, a String, an object with a `valueOf`, `undefined` and a BigInt, and
// every one of them has to answer what 13.15.3 says it answers.
//
// A BigInt is spelled below, which turns the numeric arm off for the whole
// module (`--assume-no-bigint`'s whole-program scan, `lowerer.h`): `*` and `-`
// stay boxed here, so this case exercises the closure over boxed ARITHMETIC.
// `guarded_region_unbox_rewrite` is the BigInt-free half that exercises the
// checked `unbox.f64` candidates instead.

const log = [];

const numberish = {
  valueOf() {
    log.push('v');
    return 10;
  },
};

function combine(x) {
  const a = x * 2;
  const b = x - 1;
  return a + b;
}

// 6 + 2. The guard holds and the fast copy runs.
console.log(combine(3));

// 13.15.3 asks ToNumeric of each operand: ToNumber("4") is 4, so 8 + 3. The
// guard fails on the entry edge and the ORIGINAL function runs; the answer is
// the same one it always gave.
console.log(combine('4'));

// 7.1.1 OrdinaryToPrimitive runs `valueOf` once per operator use, and NOTHING
// runs because of the guard: `is.number` reads bits. So the log has exactly two
// entries, in source order — `a` before `b`.
log.length = 0;
console.log(combine(numberish));
console.log(log.join(','));

// ToNumber(undefined) is NaN, and NaN + NaN is NaN.
console.log(combine(undefined));

// 13.15.3 throws when one operand is a BigInt and the other is not. The guard
// fails first — a BigInt's tag is above the number range — so the throw comes
// from the slow copy, which is the code that would have thrown anyway.
try {
  combine(10n);
  console.log('no throw');
} catch (e) {
  console.log(e instanceof TypeError);
}

// A parameter that is BOTH a guarded candidate and an object elsewhere. On the
// fast copy `x` is a Number, and a property read on a Number is legal — it goes
// to Number.prototype, which has no `length`, so it is `undefined` and
// `undefined + 20` is NaN. On the slow copy `x` is a String and `.length` is
// its length.
function lengthPlus(x) {
  const y = x * 2;
  return x.length + y;
}

console.log(lengthPlus(10));
console.log(lengthPlus('12'));

// The guard is on a VALUE and holds nothing across calls: a Number after a
// String is the fast copy again.
console.log(combine(3));
