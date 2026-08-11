#pragma once

#include <cstdint>
#include <vector>

// The character-level questions a pattern asks: which code units a class
// escape stands for, and what `i` does to a comparison.
//
// It is its own file because both halves of the module need it and neither
// owns it: the parser turns `\d` into a range list here, and the matcher asks
// here whether two units are the same under `i`. One answer, so the set the
// parser builds and the set the matcher tests can never disagree.

namespace bronze::regex {

// An inclusive range of UTF-16 CODE UNITS. bronze has no `u` flag
// (docs/0024), so a pattern never speaks about code points above 0xFFFF and
// a surrogate is an ordinary unit here, matched on its own.
struct Range {
    uint32_t lo = 0;
    uint32_t hi = 0;
};

using RangeList = std::vector<Range>;

constexpr uint32_t kMaxUnit = 0xFFFF;

void addRange(RangeList& list, uint32_t lo, uint32_t hi);
// Sorts and merges. Every list a class holds is normalized before the matcher
// sees it, so a membership test is a scan over disjoint ranges.
void normalizeRanges(RangeList& list);
// The complement over [0, 0xFFFF]. `\D` is built this way rather than being a
// flag, so a negated class escape inside a class (`[\D]`) is the same object
// as one outside it.
RangeList complementRanges(const RangeList& list);
bool rangesContain(const RangeList& list, uint32_t unit);

// The three class escapes of 22.2.2.9. `\w` grows two members when `i` is set
// and `u` is not — U+017F LATIN SMALL LETTER LONG S and U+212A KELVIN SIGN,
// whose canonicalizations are `s` and `k` — which is the one place the flag
// changes a SET rather than a comparison (22.2.2.7.1 WordCharacters).
const RangeList& digitRanges();
const RangeList& spaceRanges();
const RangeList& wordRanges(bool ignoreCase);

// Canonicalize (22.2.2.9) for the units bronze has case data for: ASCII and
// the Latin-1 supplement, exactly, including the two members whose uppercase
// leaves Latin-1 (U+00B5 MICRO SIGN and U+00FF) and the one whose uppercase is
// two units and therefore does not apply (U+00DF). Everything else is returned
// unchanged, which is correct for every unit `isUnknownCasedUnit` rejects.
uint16_t canonicalize(uint16_t unit, bool ignoreCase);

// Does this unit live in a block that carries case mappings bronze has no
// table for? A pattern that spells such a character under `i` is a named
// error, because answering "no match" for `/Ω/i` against `ω` would
// be a silent wrong answer — and answering it for CJK, Hebrew or an emoji
// would be a hard error for nothing, since none of them has a case at all.
bool isUnknownCasedUnit(uint32_t unit);

// Every unit at or below 0xFF whose canonicalization is `cc`. CharacterSetMatcher
// asks whether the SET holds any member that canonicalizes to the input's
// canonicalization (22.2.2.7.1), which is not the same as asking whether the
// input's canonicalization is in the set: `/[µ]/i` matches U+039C, whose
// canonicalization no Latin-1 character equals. Answering it needs the reverse
// direction of the table, which is what this is.
const std::vector<uint16_t>& caseCandidates(uint16_t cc);

}  // namespace bronze::regex
