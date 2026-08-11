#pragma once

#include <cstddef>

namespace bronze {

// JS ToString(Number) per ECMA-262 (Number::toString, radix 10): full
// decimal for magnitudes in [1e-6, 1e21), scientific outside, "0" for
// both zeros, "NaN"/"Infinity" spelled out. std::to_chars alone is NOT
// this — its shortest form flips to scientific earlier (e.g. 3000000 ->
// "3e+06" where node prints "3000000"), and the oracle compares bytes.
// `out` must hold at least 32 bytes; returns the number of bytes written.
size_t formatJsNumber(double x, char* out);

// The SHORTEST decimal digits that round-trip to `x`, and the exponent that
// places them: x == 0.d1d2...dk * 10^(exp10 + 1), i.e. the spec's
// `s × 10^(n − k)` with `n = exp10 + 1`. `x` must be finite and non-zero;
// `digits` must hold at least 20 bytes and is NOT null-terminated.
//
// It is exposed because `Number.prototype.toExponential()` with no argument
// is defined by exactly this question — 21.1.3.2 step 10.b asks for the
// smallest `f` whose digits still round-trip — and two implementations of
// "shortest round-trip" would be two chances to print a different number.
void jsShortestDigits(double x, char* digits, int& count, int& exp10);

}  // namespace bronze
