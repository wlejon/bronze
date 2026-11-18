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

// The Script of every code point (UAX #24), in exactly the shape the
// General_Category runs take and for the same reason: Scripts.txt gives every
// code point it does not list the value `Unknown`, so the runs partition
// [0, 0x10FFFF] and `\p{Script=Unknown}` names the rest.
struct ScriptRun {
    uint32_t start;
    uint16_t script;
};

extern const ScriptRun kScriptRuns[];
constexpr uint32_t kScriptRunCount = 1708;

// The 172 script values by their canonical long name, sorted, so the index
// `ScriptRun::script` carries is stable across regenerations.
extern const char* const kScriptNames[];
constexpr uint32_t kScriptCount = 172;

// Every spelling PropertyValueAliases.txt gives a script -- the four-letter
// code, the long name, and the occasional third alias (`Qaac` for Coptic) --
// sorted by name. 22.2.1 matches a property value EXACTLY, so this list is the
// whole of what a pattern may write.
struct ScriptAlias {
    const char* name;
    uint16_t script;
};

extern const ScriptAlias kScriptAliases[];
constexpr uint32_t kScriptAliasCount = 338;

// Script_Extensions, as the OVERRIDES it is: ScriptExtensions.txt lists only
// the code points whose scx differs from their sc, and every other code point's
// scx is the one-element set holding its Script. `set` is an offset into
// `kScxScripts` and `count` is the length there. Sets are shared, so two
// ranges with the same scripts name one run of the table.
struct ScxRange {
    uint32_t first;
    uint32_t last;
    uint32_t set;
    uint32_t count;
};

extern const ScxRange kScxRanges[];
constexpr uint32_t kScxRangeCount = 204;
extern const uint16_t kScxScripts[];
constexpr uint32_t kScxScriptCount = 501;

}  // namespace bronze::regex::data
