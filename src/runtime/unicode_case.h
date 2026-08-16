#pragma once

#include <cstdint>
#include <vector>

namespace bronze::runtime::unicode {

// Default Case Conversion (UCD 3.13), which is the operation ECMA-262 11.1.3
// names and 22.1.3.28 / 22.1.3.30 apply. FULL, not simple: the result may be
// longer than the input, because "ß" uppercases to "SS" and "ﬁ" to "FI".
//
// The currency is a sequence of UTF-16 code UNITS, which is what a String is
// (6.1.4) and what `builtin_string.cpp` passes around. The conversion itself is
// defined over code POINTS, so these decode and re-encode; an unpaired
// surrogate is not a code point pair and passes through unchanged, which is the
// same reading 11.1.4 CodePointAt gives it.
//
// LOCALE tailorings are deliberately absent. SpecialCasing.txt marks them with
// a language ID and the generator drops those lines, so what is here is the
// language-independent mapping and nothing else — which is exactly what
// `toUpperCase` is defined to apply, and what `toLocaleUpperCase` falls back to
// with no locale.
std::vector<uint16_t> toUpperFull(const std::vector<uint16_t>& units);
std::vector<uint16_t> toLowerFull(const std::vector<uint16_t>& units);

}  // namespace bronze::runtime::unicode
