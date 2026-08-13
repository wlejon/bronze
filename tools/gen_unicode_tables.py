#!/usr/bin/env python3
"""Generate the Unicode tables src/regex needs, as ordinary checked-in C++.

Run once, commit the output. The build never invokes this: bronze does not
depend on Python, and a table that is regenerated as part of a build is a
table nobody has read.

    python3 tools/gen_unicode_tables.py

The UCD version is asserted, not reported, so a run on a machine carrying a
different one stops instead of quietly producing different bytes than the
files already in the tree. Everything written below is sorted and formatted
to fixed widths, so a correct rerun is a no-op diff.

Two tables come out of it.

General_Category is read straight from `unicodedata.category`, which is the
UCD field itself, and is written as the RUNS the property forms over the whole
code space -- unassigned code points included, since Cn is a General_Category
value like any other and `\\p{Cn}` names it. The runs partition
[0, 0x10FFFF], so a lookup is one binary search and no code point can be
missing from or claimed by two categories.

Simple case folding is DERIVED, because Python exposes no scf directly, and
the derivation is the delicate part of this script. It is set out at
`simple_case_fold` below, together with the checks that make it defensible;
those checks run on every generation and abort it if they ever fail.
"""

import os
import sys
import unicodedata

# The version these tables were generated from. Bump it only together with a
# regeneration -- the point of the assert is that the committed bytes and the
# UCD they came from can never disagree.
REQUIRED_UNIDATA_VERSION = "16.0.0"

MAX_CODE_POINT = 0x10FFFF

BANNER = """// GENERATED FILE -- DO NOT EDIT BY HAND.
//
// Written by tools/gen_unicode_tables.py from the Unicode Character Database
// version {version}, as carried by Python's `unicodedata`. To change anything
// here, change the generator and rerun it:
//
//     python3 tools/gen_unicode_tables.py
//
// The generator asserts the UCD version it reads, so a rerun either reproduces
// these bytes or stops.
"""


def fail(message):
    sys.stderr.write("gen_unicode_tables: %s\n" % message)
    sys.exit(1)


# ---------------------------------------------------------------------------
# Simple case folding
# ---------------------------------------------------------------------------
#
# CaseFolding.txt gives four statuses. C is the common mapping shared by the
# simple and the full folding; F is the full one, which may grow a string; S is
# a single-character mapping given only where it DIFFERS from that code point's
# F mapping; T is the Turkic variant. Simple case folding -- the scf that
# ECMA-262 22.2.2.9 Canonicalize applies under `u` and `i` -- is C + S, and
# deliberately not T, because 22.2.2.9 asks for "the simple or common case
# folding" and the Turkic table is neither.
#
# Python gives us the full folding: `str.casefold()` is C + F. That yields C
# directly, since a C mapping is exactly a folding of length one. It does not
# yield S, and this is where a plausible-sounding shortcut is WRONG: "where the
# full folding is longer than one code point, scf leaves the character alone"
# holds only for the code points that have no S line. U+1E9E LATIN CAPITAL
# LETTER SHARP S folds fully to "ss" and simply to U+00DF; the shortcut would
# leave it alone, and `/ẞ/ui` would then not match "ß".
#
# S is recovered from the simple lowercase mapping. `str.lower()` is the FULL
# lowercase mapping, which differs from the simple one at exactly one code
# point -- U+0130, whose full lowercase is two code points -- and that one
# difference is load-bearing rather than an annoyance: U+0130's single-character
# lowercase is U+0069, but its case FOLDING to U+0069 is status T, so scf must
# leave it alone, and reading `lower()` as "multi-character, therefore no S"
# reaches that answer without knowing anything about Turkic.
#
# So the derivation is:
#
#   scf(c) = casefold(c)      when casefold(c) is one code point   (status C)
#          = lower(c)         when it is one code point and not c  (status S)
#          = c                otherwise
#
# and it is checked, not asserted, by `check_fold_derivation` below.


def simple_case_fold(cp):
    ch = chr(cp)
    folded = ch.casefold()
    if len(folded) == 1:
        return ord(folded)
    lowered = ch.lower()
    if len(lowered) == 1 and lowered != ch:
        return ord(lowered)
    return cp


def check_fold_derivation(folds):
    """Make the derivation defensible, on every generation.

    The first check is the one that matters. Full case folding is the ground
    truth we do have, and any correct simple folding has to agree with it about
    which characters are case-equivalent: scf may map two characters to
    different code points, but it must never map a character to one whose FULL
    folding differs, because that would move it into another equivalence class
    entirely. Running it over all 1114112 code points is what turns "the rule
    sounds right" into "the rule cannot have merged or split a class".

    The second is idempotence, which the specification's use of scf assumes --
    a pattern's character and the input's are canonicalized independently and
    then compared, so a fold that moved twice would compare unequal.

    The third pins the Turkic exclusion by name, since it is the one place the
    derivation depends on Python's full lowercase being longer than one code
    point rather than on anything about folding.
    """
    for cp in range(MAX_CODE_POINT + 1):
        if chr(simple_case_fold(cp)).casefold() != chr(cp).casefold():
            fail("scf(U+%04X) leaves a different full-fold class; the derivation is "
                 "unsound and no table should be written from it" % cp)
    for cp in range(MAX_CODE_POINT + 1):
        folded = simple_case_fold(cp)
        if simple_case_fold(folded) != folded:
            fail("scf is not idempotent at U+%04X" % cp)
    # Turkic: `I`/`i` fold together (status C), and the dotted and dotless
    # capitals do NOT fold to them (status T, excluded).
    turkic = {0x0049: 0x0069, 0x0069: 0x0069, 0x0130: 0x0130, 0x0131: 0x0131}
    for cp, want in turkic.items():
        got = simple_case_fold(cp)
        if got != want:
            fail("Turkic exclusion broken: scf(U+%04X) is U+%04X, expected U+%04X"
                 % (cp, got, want))
    # The two characters ECMA-262 22.2.2.7.1 step 3 exists for: under `u` and
    # `i` they, and only they, join WordCharacters.
    basic = set(range(0x30, 0x3A)) | set(range(0x41, 0x5B)) | {0x5F} | set(range(0x61, 0x7B))
    extra = sorted(src for src, dst in folds if dst in basic and src not in basic)
    if extra != [0x017F, 0x212A]:
        fail("the characters folding into the basic word set are %s, not the "
             "U+017F and U+212A WordCharacters is written around"
             % ["U+%04X" % c for c in extra])
    # `\w` and `\b` read a code UNIT rather than decoding, which is only sound
    # while nothing outside the BMP folds into the basic set. Checked here as
    # well as in tests/regex, because this is where it could change.
    if any(c > 0xFFFF for c in extra):
        fail("a code point outside the BMP folds into the basic word set")


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------


def write(path, text):
    # Newline "\n" explicitly: these are checked-in sources and must not pick
    # up CRLF from the platform running the generator.
    with open(path, "w", encoding="ascii", newline="\n") as handle:
        handle.write(text)
    sys.stdout.write("wrote %s (%d lines)\n" % (path, text.count("\n")))


def chunk(items, per_line):
    for i in range(0, len(items), per_line):
        yield items[i:i + per_line]


def gc_runs():
    """(start, category) for every run of one General_Category, ascending."""
    runs = []
    previous = None
    for cp in range(MAX_CODE_POINT + 1):
        category = unicodedata.category(chr(cp))
        if category != previous:
            runs.append((cp, category))
            previous = category
    return runs


def emit_header(version, aliases, run_count, fold_count):
    return BANNER.format(version=version) + """
#pragma once

#include <cstdint>

namespace bronze::regex::data {{

// The General_Category of every code point, as the RUNS the property forms:
// `start` is the first code point of a run and `category` indexes
// `kGcAliases`. A run ends where the next begins, so there is no end field and
// no way to write a gap: the runs partition [0, 0x10FFFF], unassigned code
// points included, because `Cn` is a General_Category value and `\\p{{Cn}}`
// names it.
struct GcRun {{
    uint32_t start;
    uint8_t category;
}};

extern const GcRun kGcRuns[];
constexpr uint32_t kGcRunCount = {run_count};

// The {alias_count} General_Category values, by the two-letter alias UAX #44 gives them,
// in the order `GcRun::category` indexes. Sorted, so the index is stable
// across regenerations.
extern const char* const kGcAliases[];
constexpr uint32_t kGcAliasCount = {alias_count};

// Simple case folding -- CaseFolding.txt statuses C and S, which is what
// ECMA-262 22.2.2.9 applies under `u` and `i`. Only the code points scf does
// NOT leave alone are here, ascending by `from`, so a lookup that finds
// nothing is an identity fold.
struct FoldEntry {{
    uint32_t from;
    uint32_t to;
}};

extern const FoldEntry kSimpleCaseFolds[];
constexpr uint32_t kSimpleCaseFoldCount = {fold_count};

}}  // namespace bronze::regex::data
""".format(run_count=run_count, alias_count=len(aliases), fold_count=fold_count)


def emit_gc(version, runs, aliases):
    index = {alias: i for i, alias in enumerate(aliases)}
    cells = ["{0x%06X,%2u}," % (start, index[category]) for start, category in runs]
    body = "\n".join("    " + " ".join(line) for line in chunk(cells, 6))
    names = "\n".join("    " + " ".join('"%s",' % a for a in line) for line in chunk(aliases, 10))
    return BANNER.format(version=version) + """
#include "regex/unicode_data.h"

namespace bronze::regex::data {

const char* const kGcAliases[] = {
%s
};

const GcRun kGcRuns[] = {
%s
};

}  // namespace bronze::regex::data
""" % (names, body)


def emit_scf(version, folds):
    # Four per line rather than five: the .clang-format here sets a 100-column
    # limit, and a fifth pair crosses it.
    cells = ["{0x%06X,0x%06X}," % pair for pair in folds]
    body = "\n".join("    " + " ".join(line) for line in chunk(cells, 4))
    return BANNER.format(version=version) + """
#include "regex/unicode_data.h"

namespace bronze::regex::data {

const FoldEntry kSimpleCaseFolds[] = {
%s
};

}  // namespace bronze::regex::data
""" % body


def main():
    version = unicodedata.unidata_version
    if version != REQUIRED_UNIDATA_VERSION:
        fail("this generator is pinned to UCD %s and the interpreter carries %s. "
             "Regenerating from a different UCD would change the committed tables "
             "silently, so it stops here; use a Python whose unicodedata is %s, or "
             "change REQUIRED_UNIDATA_VERSION deliberately and regenerate everything."
             % (REQUIRED_UNIDATA_VERSION, version, REQUIRED_UNIDATA_VERSION))
    if sys.maxunicode != MAX_CODE_POINT:
        fail("this interpreter's strings top out at U+%04X, so it cannot see the "
             "whole code space" % sys.maxunicode)

    folds = [(cp, simple_case_fold(cp))
             for cp in range(MAX_CODE_POINT + 1)
             if simple_case_fold(cp) != cp]
    check_fold_derivation(folds)

    runs = gc_runs()
    aliases = sorted({category for _, category in runs})
    if len(aliases) != 30:
        fail("expected the 30 General_Category values, found %d: %s" % (len(aliases), aliases))

    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(os.path.dirname(here), "src", "regex")
    write(os.path.join(out, "unicode_data.h"),
          emit_header(version, aliases, len(runs), len(folds)))
    write(os.path.join(out, "unicode_data_gc.cpp"), emit_gc(version, runs, aliases))
    write(os.path.join(out, "unicode_data_scf.cpp"), emit_scf(version, folds))


if __name__ == "__main__":
    main()
