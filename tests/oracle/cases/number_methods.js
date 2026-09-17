// `Number.prototype.toFixed`, `toExponential`, `toPrecision` and
// `toString(radix)` (ECMA-262 21.1.3).
//
// All four are defined on the EXACT real number the double denotes, not on the
// shortest decimal that round-trips to it, so every digit here comes from
// integer arithmetic on the mantissa and exponent (`src/runtime/exact_decimal`)
// and none from printf or a to_chars round-trip. The cases everyone quotes as
// engine bugs are the correct answers, and they are the reason this needed its
// own derivation: getting them nearly right is a SILENT wrong answer in exactly
// the code — money, report columns — that reaches for these methods.
//
// What this case pins, from ECMA-262 21.1.3.3 (toFixed), 21.1.3.2
// (toExponential), 21.1.3.5 (toPrecision) and 21.1.3.6 (toString):
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
//    sign in front rather than in the digits. A power-of-two radix prints
//    the dyadic fraction exactly; any other radix prints V8's digit count
//    (stop below half the gap to the next double, round half-to-even with
//    carry), so a random id's `.toString(36)` spells what Chromium spells.
console.log((1.005).toFixed(2), (2.675).toFixed(2), (8.575).toFixed(2));
console.log((1.5).toFixed(0), (2.5).toFixed(0), (-1.5).toFixed(0));
console.log((1).toFixed(3), (0).toFixed(2), (-0.0004).toFixed(2));
console.log((1234.5678).toFixed(0), (1234.5678).toFixed(4));
console.log((1e21).toFixed(2), (1e-7).toFixed(2));
console.log((123.456).toExponential(2), (0).toExponential(1));
console.log((123.456).toPrecision(2), (123.456).toPrecision(6));
console.log((255).toString(16), (255).toString(2), (0.5).toString(2));
console.log((-255).toString(16), (3735928559).toString(16));
console.log((0.1).toString(36), (0.5).toString(3), (1 / 3).toString(3));
console.log((255.5).toString(36), (0.7).toString(36), (123.456).toString(7));
console.log((-0.1).toString(36), (1e-7).toString(36), (0.9999999999999999).toString(3));
console.log((0.5 + Number.EPSILON).toString(36), (1 - Number.EPSILON / 2).toString(36), (7 - Number.EPSILON * 4).toString(36));
