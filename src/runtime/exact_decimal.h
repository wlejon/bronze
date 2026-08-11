#pragma once

#include <string>

namespace bronze::runtime {

// The exact decimal expansion of a double.
//
// This file exists because `Number.prototype.toFixed` and its two relatives
// are defined on the REAL NUMBER a double denotes, not on the shortest
// decimal that round-trips to it, and the two disagree constantly. ECMA-262
// 21.1.3.3 asks for "the integer n for which n / 10^f - x is closest to
// zero"; the double nearest 1.005 is 1.00499999999999989341858963598497211933
// exactly, so that n is 100 and `(1.005).toFixed(2)` is "1.00". Anything that
// reaches for printf, std::format or a to_chars round-trip answers "1.01" —
// a silent wrong answer in precisely the code (money, report columns) that
// calls toFixed at all.
//
// So every digit below comes from exact integer arithmetic on the double's
// own mantissa and exponent, with no floating-point step in the middle.

// The decimal digits of round(|x| * 10^k), with a tie resolved AWAY from zero
// — which is what "if there are two such n, pick the larger n" means once the
// sign has been split off, as all three methods do before they ask. No sign,
// no radix point, no leading zero except for the value zero itself.
//
// `x` must be finite. `k` is the caller's scale and is bounded by the
// methods' own 0..100 argument range plus the double's exponent range.
std::string exactScaledDigits(double x, int k);

// |trunc(x)| in `radix` (2..36), exactly. Used by 21.1.3.6's non-decimal
// spelling, where the integer part is not an approximation of anything.
std::string exactIntegerDigits(double x, int radix);

// The fractional part of |x| in `radix`, which must be a POWER OF TWO. A
// double's fraction is a dyadic rational, so in a power-of-two radix the
// expansion is finite and every digit is exact — and in any other radix it
// generally is not, which is why this function refuses to be asked. Empty
// when the fraction is zero; no leading ".".
std::string exactDyadicFractionDigits(double x, int radix);

}  // namespace bronze::runtime
