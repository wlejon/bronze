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

// An inclusive range of UTF-16 CODE UNITS. bronze has no `u` flag, so a pattern
// never speaks about code points above 0xFFFF and a surrogate is an ordinary
// unit here, matched on its own.
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

// The three class escapes of 22.2.2.9. `\w` is 22.2.2.7.1 WordCharacters: the
// basic sixty-three, plus every unit that is not one of them whose
// `canonicalize` IS one of them — a set the specification asserts is EMPTY
// unless `u` and `i` are both set. bronze has no `u`, so `\w` is the same
// sixty-three in both modes, and `ignoreCase` still selects the set because it
// selects which `canonicalize` derives it.
//
// The derivation is what makes the two agree. U+017F and U+212A look like they
// belong — each uppercases to a word character — but `canonicalize` returns
// both unchanged (22.2.2.9 step 4 keeps a non-ASCII unit whose uppercase is
// ASCII), so `/ſ/i` does not match "s" and `\w` must not hold "ſ" either.
const RangeList& digitRanges();
const RangeList& spaceRanges();
const RangeList& wordRanges(bool ignoreCase);

// Canonicalize (22.2.2.9) for the units bronze has case data for. There is no
// Unicode data file behind it: each block is written as the RULE that
// generated that part of the Default Case Conversion table, plus the members
// that break the rule, so every mapping in it can be read and checked without
// one. The blocks are ASCII and Latin-1, Latin Extended-A, Greek and Coptic,
// Cyrillic with its supplement, and the two Armenian letter runs. Everything
// else is returned unchanged, which is correct for every unit
// `isUnknownCasedUnit` rejects.
uint16_t canonicalize(uint16_t unit, bool ignoreCase);

// Does this unit live in a block that carries case mappings bronze has no
// table for? A pattern that spells such a character under `i` is a named
// error, because answering "no match" for `/Ω/i` against `ω` would
// be a silent wrong answer — and answering it for CJK, Hebrew or an emoji
// would be a hard error for nothing, since none of them has a case at all.
//
// Shrinking this is what adding a block to `canonicalize` means. The two must
// move together: a unit that is folded but still refused is a hard error for
// nothing, and a unit that is neither is the silent wrong answer.
bool isUnknownCasedUnit(uint32_t unit);

// The lowest unit in [lo, hi] that `isUnknownCasedUnit` refuses, or false when
// the range holds none. A class RANGE has to ask this rather than test its two
// endpoints: `[ÿ- ]` names every refused unit between them without
// spelling one, and skipping the fold there is the silent wrong answer the
// refusal exists to prevent. The refused set is a short sorted list of blocks,
// so this is an interval overlap and not a walk over the range — `[\0-￿]`
// costs the same as `[a-b]`, and is now correctly a named error.
bool firstUnknownCasedUnitInRange(uint32_t lo, uint32_t hi, uint32_t& out);

// Every unit whose canonicalization is `cc` and is not `cc` itself.
// CharacterSetMatcher asks whether the SET holds any member that canonicalizes
// to the input's canonicalization (22.2.2.7.1), which is not the same as
// asking whether the input's canonicalization is in the set: `/[µ]/i` matches
// U+039C, whose canonicalization no Latin-1 character equals, and
// `/[α-ω]/i` matches "Γ" only because U+03B3 canonicalizes the same
// way it does. Answering that needs the reverse direction of the table, which
// is what this is.
const std::vector<uint16_t>& caseCandidates(uint16_t cc);

}  // namespace bronze::regex
