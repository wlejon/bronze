// GENERATED FILE -- DO NOT EDIT BY HAND.
//
// Written by tools/gen_unicode_tables from the Unicode Character Database
// version 16.0.0 vendored under tools/ucd/. To change anything here, change the
// generator and rerun it:
//
//     tools/gen_unicode_tables
//
// The generator asserts the UCD version it reads, so a rerun either reproduces
// these bytes or stops.

#pragma once

#include <cstdint>

namespace bronze::runtime::unicode {

// Default Case Conversion (UCD 3.13), which is what ECMA-262 11.1.3 makes
// String.prototype.toUpperCase and toLowerCase apply. This is a DIFFERENT
// operation from the simple case folding in src/regex/unicode_data.h and the
// tables are not interchangeable: scf(U+1E9E) is U+00DF and so is
// toLowerCase(U+1E9E), but toUpperCase(U+00DF) is the two code points "SS",
// which no folding table can spell.

// A 1:1 mapping, from UnicodeData.txt field 12 (uppercase) or 13 (lowercase).
// Only the code points the mapping does NOT leave alone are here, ascending by
// `from`, so a lookup that finds nothing is an identity mapping.
struct CaseEntry {
    uint32_t from;
    uint32_t to;
};

extern const CaseEntry kSimpleUppercase[];
constexpr uint32_t kSimpleUppercaseCount = 1477;
extern const CaseEntry kSimpleLowercase[];
constexpr uint32_t kSimpleLowercaseCount = 1460;

// A 1:MANY mapping, from SpecialCasing.txt's unconditional lines. `count` is 2
// or 3; a mapping of length 1 is never here, because it would be the simple
// mapping above and two copies of one fact is one too many. These SHADOW the
// simple tables: a code point in both is answered from this one.
struct FullCaseEntry {
    uint32_t from;
    uint32_t to[3];
    uint8_t count;
};

extern const FullCaseEntry kFullUppercase[];
constexpr uint32_t kFullUppercaseCount = 102;
extern const FullCaseEntry kFullLowercase[];
constexpr uint32_t kFullLowercaseCount = 1;

// Cased and Case_Ignorable, from DerivedCoreProperties.txt, as disjoint
// ascending intervals. They are here for ONE caller: the Final_Sigma condition
// of SpecialCasing.txt, which is the only language-independent context rule in
// default casing and is defined in terms of exactly these two properties.
struct PropRange {
    uint32_t first;
    uint32_t last;
};

extern const PropRange kCasedRanges[];
constexpr uint32_t kCasedRangeCount = 159;
extern const PropRange kCaseIgnorableRanges[];
constexpr uint32_t kCaseIgnorableRangeCount = 452;

}  // namespace bronze::runtime::unicode
