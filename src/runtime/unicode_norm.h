#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bronze::runtime::unicode {

// Unicode Normalization Forms (UAX #15), which ECMA-262 22.1.3.16
// String.prototype.normalize applies.
//
// The currency is UTF-16 code UNITS, as for the case conversions in
// unicode_case.h; the algorithm is over code POINTS, so these decode and
// re-encode. An unpaired surrogate is a code point equal to itself, which has
// combining class 0, no decomposition and composes with nothing -- so it
// passes through every form unchanged, a starter that blocks what follows it.
enum class NormForm : uint8_t { NFC, NFD, NFKC, NFKD };

// True when every code unit is below the bound under which `form` is the
// identity (unicode_norm_data.h's quick-check bounds), so the string is
// already normalized and needs no decoding. False says nothing either way.
bool isTriviallyNormalized(const uint16_t* units, size_t length, NormForm form);
bool isTriviallyNormalizedLatin1(const char* bytes, size_t length, NormForm form);

std::vector<uint16_t> normalize(const std::vector<uint16_t>& units, NormForm form);

// Canonical_Combining_Class of a code point; 0 for a starter.
uint8_t combiningClass(uint32_t cp);

}  // namespace bronze::runtime::unicode
