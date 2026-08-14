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

namespace bronze::regex::data {

// The General_Category of every code point, as the RUNS the property forms:
// `start` is the first code point of a run and `category` indexes
// `kGcAliases`. A run ends where the next begins, so there is no end field and
// no way to write a gap: the runs partition [0, 0x10FFFF], unassigned code
// points included, because `Cn` is a General_Category value and `\p{Cn}`
// names it.
struct GcRun {
    uint32_t start;
    uint8_t category;
};

extern const GcRun kGcRuns[];
constexpr uint32_t kGcRunCount = 4099;

// The 30 General_Category values, by the two-letter alias UAX #44 gives them,
// in the order `GcRun::category` indexes. Sorted, so the index is stable
// across regenerations.
extern const char* const kGcAliases[];
constexpr uint32_t kGcAliasCount = 30;

// Simple case folding -- CaseFolding.txt statuses C and S, which is what
// ECMA-262 22.2.2.9 applies under `u` and `i`. Only the code points scf does
// NOT leave alone are here, ascending by `from`, so a lookup that finds
// nothing is an identity fold.
struct FoldEntry {
    uint32_t from;
    uint32_t to;
};

extern const FoldEntry kSimpleCaseFolds[];
constexpr uint32_t kSimpleCaseFoldCount = 1484;

}  // namespace bronze::regex::data
