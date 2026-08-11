// BLOCKED: `unsupported member: Number.prototype.toFixed` — reached as
// `unsupported member: toFixed on a number`. docs/0021 landed the `Number`
// NAMESPACE (the predicates, the constants, `parseInt`/`parseFloat`) and
// stopped there deliberately.
//
// The blocker is that these are methods on the WRAPPER, not statics: `(1.005
// ).toFixed(2)` is a member access on a primitive, which means the number
// prototype, and bronze's `String.prototype` methods are the only precedent
// for that shape (docs/0011). That part is plumbing. The part that is not is
// the formatting itself: 21.1.3.3 toFixed is NOT `printf("%.*f")` and is not
// `std::to_chars` either — it is defined on the exact mathematical value of
// the double, which is why `(1.005).toFixed(2)` is "1.00" and not "1.01" (the
// double nearest 1.005 is slightly below it), and why `(1e21).toFixed(2)`
// falls back to ToString entirely. `toPrecision` and `toExponential` have the
// same shape and the same trap. Getting that wrong is a SILENT wrong answer
// in exactly the code — money, report columns — that reaches for it, so it
// needs its own derivation and its own case rather than a corner of a chunk
// about iteration.
//
// `toString(radix)` is here for the same reason: 21.1.3.6 with a radix other
// than 10 is its own algorithm over the fraction bits, not a division loop.
//
// What this case pins when it lands, from ECMA-262 21.1.3.3 (toFixed),
// 21.1.3.2 (toExponential), 21.1.3.5 (toPrecision) and 21.1.3.6 (toString):
//
// 1. Rounding is on the DOUBLE, so the cases everyone quotes as bugs are the
//    correct answers: 1.005 -> "1.00", 2.675 -> "2.67", 8.575 -> "8.57".
// 2. Halfway cases that ARE exact round half away from zero: 1.5 -> "2",
//    2.5 -> "3", -1.5 -> "-2".
// 3. Digits are padded, not truncated — (1).toFixed(3) is "1.000" — and the
//    SIGN survives a result whose every digit is zero, so
//    (-0.0004).toFixed(2) is "-0.00".
// 4. Above 1e21 toFixed gives up and returns ToString(x) — the one place the
//    method changes format rather than precision.
// 5. `toString(radix)` emits digits and a fraction in that radix, with the
//    sign in front rather than in the digits.
console.log((1.005).toFixed(2), (2.675).toFixed(2), (8.575).toFixed(2));
console.log((1.5).toFixed(0), (2.5).toFixed(0), (-1.5).toFixed(0));
console.log((1).toFixed(3), (0).toFixed(2), (-0.0004).toFixed(2));
console.log((1234.5678).toFixed(0), (1234.5678).toFixed(4));
console.log((1e21).toFixed(2), (1e-7).toFixed(2));
console.log((123.456).toExponential(2), (0).toExponential(1));
console.log((123.456).toPrecision(2), (123.456).toPrecision(6));
console.log((255).toString(16), (255).toString(2), (0.5).toString(2));
console.log((-255).toString(16), (3735928559).toString(16));
