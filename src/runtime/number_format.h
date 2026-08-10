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

}  // namespace bronze
