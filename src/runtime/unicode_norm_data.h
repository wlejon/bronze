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

// Unicode Normalization (UAX #15), which String.prototype.normalize
// (ECMA-262 22.1.3.16) applies. One level of each mapping, as UnicodeData.txt
// gives it: the full decomposition, the composition pairs and the derived
// part of Full_Composition_Exclusion are computed from these by
// runtime/unicode_norm.cpp, and Hangul is arithmetic and absent.

// A run of code points sharing one nonzero Canonical_Combining_Class,
// ascending and disjoint; a code point in no run is a starter (class 0).
struct CccRun {
    uint32_t first;
    uint32_t last;
    uint8_t ccc;
};

extern const CccRun kCccRuns[];
constexpr uint32_t kCccRunCount = 393;

// A decomposition STREAM, one record per code point that has a mapping, in
// ascending order:
//   step, n, m1 .. mn       the code point maps to the n code points m1..mn
//   step, 0, count, t       `count` consecutive code points map to t, t+1, ...
// `step` is the distance from the previous record's last code point (from 0
// for the first). It is read once, into a searchable index, by
// runtime/unicode_norm.cpp. Canonical and compatibility mappings are separate
// streams: a code point has at most one mapping, and NFD reads only the first.
extern const uint32_t kCanonicalDecompStream[];
constexpr uint32_t kCanonicalDecompStreamSize = 7271;
extern const uint32_t kCompatDecompStream[];
constexpr uint32_t kCompatDecompStreamSize = 9192;

// CompositionExclusions.txt's explicit list, ascending: the script-specific
// and post-composition-version exclusions. The rest of
// Full_Composition_Exclusion (singletons, non-starter decompositions) follows
// from the tables above.
extern const uint32_t kCompositionExclusions[];
constexpr uint32_t kCompositionExclusionCount = 81;

// Below each bound every code point is a starter the form leaves alone (for
// the composed forms: never excluded and never the second of a composable
// pair), so a string whose code units are all below it is already in that
// form. Every bound is below the surrogates.
constexpr uint32_t kNfdQuickBelow = 0xC0;
constexpr uint32_t kNfkdQuickBelow = 0xA0;
constexpr uint32_t kNfcQuickBelow = 0x300;
constexpr uint32_t kNfkcQuickBelow = 0xA0;

}  // namespace bronze::runtime::unicode
