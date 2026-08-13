#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// The character-level questions a pattern asks: which characters a class
// escape stands for, what `i` does to a comparison, and — since the `u` flag —
// where one character ends and the next begins.
//
// It is its own file because both halves of the module need it and neither
// owns it: the parser turns `\d` into a range list here, and the matcher asks
// here whether two characters are the same under `i`. One answer, so the set
// the parser builds and the set the matcher tests can never disagree.

namespace bronze::regex {

// An inclusive range of CODE POINTS, in both modes. 22.2.1 gives the two modes
// different alphabets — the UTF-16 code unit without `u`, the code point with
// it — but not different range arithmetic: a non-`u` pattern simply never
// builds a range whose endpoint exceeds 0xFFFF, because nothing it can spell
// denotes one. Carrying a single range set is what keeps `addRange`,
// `normalizeRanges` and `rangesContain` mode-blind, and it is honest because
// the two alphabets NEST — every code unit is a code point, and a surrogate
// stays an ordinary member of the set either way.
//
// The one operation that cannot be mode-blind is the complement, because it is
// the only one that means "everything else" and the two modes disagree about
// how much that is. So the ceiling is a parameter there and nowhere else.
struct Range {
    uint32_t lo = 0;
    uint32_t hi = 0;
};

using RangeList = std::vector<Range>;

constexpr uint32_t kMaxUnit = 0xFFFF;
constexpr uint32_t kMaxCodePoint = 0x10FFFF;

// The ceiling the alphabet of `unicode` reaches, which is what a complement is
// taken over and what a `\u{...}` escape may name.
constexpr uint32_t alphabetCeiling(bool unicode) { return unicode ? kMaxCodePoint : kMaxUnit; }

void addRange(RangeList& list, uint32_t lo, uint32_t hi);
// Sorts and merges. Every list a class holds is normalized before the matcher
// sees it, so a membership test is a scan over disjoint ranges.
void normalizeRanges(RangeList& list);
// The complement over [0, ceiling]. `\D` is built this way rather than being a
// flag, so a negated class escape inside a class (`[\D]`) is the same object
// as one outside it — and `ceiling` is `alphabetCeiling(unicode)`, because a
// `\D` under `u` that stopped at 0xFFFF would silently exclude every astral
// code point from a set whose whole meaning is "not a digit".
RangeList complementRanges(const RangeList& list, uint32_t ceiling);
bool rangesContain(const RangeList& list, uint32_t code);

// One character read out of a UTF-16 sequence, and how many code units it
// occupies. Without `u` that is always one unit and the two are the same
// question; with `u` a surrogate PAIR is one character and everything that
// steps over a character has to step two.
//
// An UNPAIRED surrogate is returned as itself, width one. It is a code point
// for matching purposes — `/^.$/u` matches a lone lead surrogate — and turning
// it into U+FFFD here would make the matcher answer about a character the
// subject does not contain.
struct CodePointStep {
    uint32_t code = 0;
    size_t width = 1;
};

// The character starting at `index`, which must be less than `input.size()`.
CodePointStep codePointAt(std::u16string_view input, size_t index, bool unicode);
// The character ENDING at `index`, which must be greater than zero: the
// backward form a lookbehind reads with. Under `u` it recognises a trailing
// surrogate at `index - 1` and steps back over its lead, so the two functions
// agree about where every character boundary is.
CodePointStep codePointBefore(std::u16string_view input, size_t index, bool unicode);

// The three class escapes of 22.2.2.9. `\w` is 22.2.2.7.1 WordCharacters: the
// basic sixty-three, plus every character that is not one of them whose
// `canonicalize` IS one of them — a set step 3 asserts is EMPTY unless `u` and
// `i` are BOTH set.
//
// Both sides of that assertion are now live, which is why the extra set is
// DERIVED and never written down. Without `u`, Canonicalize is the uppercase
// mapping and step 4 keeps a non-ASCII character whose uppercase is ASCII, so
// U+017F and U+212A canonicalize to themselves and the extra set comes out
// empty. With `u` and `i` it is simple case folding, U+017F folds to `s` and
// U+212A to `k`, and the extra set is exactly those two. The same two
// characters, on opposite sides of the answer, decided by the table rather
// than by a list — which is the only way `/\w/` and `/ſ/i` can be made to
// agree about one.
const RangeList& digitRanges();
const RangeList& spaceRanges();
const RangeList& wordRanges(bool ignoreCase, bool unicode);

// Canonicalize (22.2.2.9), whose whole content is that there are two tables
// and the flags pick one.
//
// With `u` and `i` (step 1) it is simple case folding, which comes from the
// UCD by way of the generated tables — see `regex/unicode.h` — and covers the
// whole code space, astral characters included.
//
// With `i` alone (step 3) it is `toUppercase` plus step 4's guard, and there
// bronze has no data file: each block is written in `chars.cpp` as the RULE
// that generated that part of the Default Case Conversion table plus the
// members that break the rule, so every mapping can be read and checked
// without one. The blocks are ASCII and Latin-1, Latin Extended-A, Greek and
// Coptic, Cyrillic with its supplement, and the two Armenian letter runs;
// everything else is returned unchanged, which is correct for every unit
// `isUnknownCasedUnit` rejects and is the reason that refusal exists.
uint32_t canonicalize(uint32_t code, bool ignoreCase, bool unicode);

// Does this unit live in a block that carries case mappings bronze has no
// UPPERCASE table for? A pattern that spells such a character under `i`
// WITHOUT `u` is a named error, because answering "no match" for a capital
// omega against a small one would be a silent wrong answer — and answering it
// for CJK, Hebrew or an emoji would be a hard error for nothing, since none of
// them has a case at all.
//
// It has nothing to say about `u` and `i` together. Simple case folding is
// generated from the UCD and has no holes, so a caller that consulted this
// there would refuse patterns bronze can answer exactly — which is why every
// caller guards it with the flag rather than asking it about every mode.
//
// Shrinking this is what adding a block to the uppercase table means. The two
// must move together: a unit that is folded but still refused is a hard error
// for nothing, and a unit that is neither is the silent wrong answer.
bool isUnknownCasedUnit(uint32_t unit);

// The lowest unit in [lo, hi] that `isUnknownCasedUnit` refuses, or false when
// the range holds none. A class RANGE has to ask this rather than test its two
// endpoints: `[ÿ- ]` names every refused unit between them without
// spelling one, and skipping the fold there is the silent wrong answer the
// refusal exists to prevent. The refused set is a short sorted list of blocks,
// so this is an interval overlap and not a walk over the range — `[\0-￿]`
// costs the same as `[a-b]`, and is now correctly a named error.
bool firstUnknownCasedUnitInRange(uint32_t lo, uint32_t hi, uint32_t& out);

// Every character whose canonicalization is `cc` and is not `cc` itself.
// CharacterSetMatcher asks whether the SET holds any member that canonicalizes
// to the input's canonicalization (22.2.2.7.1), which is not the same as
// asking whether the input's canonicalization is in the set: `/[µ]/i` matches
// U+039C, whose canonicalization no Latin-1 character equals, and
// `/[α-ω]/i` matches "Γ" only because U+03B3 canonicalizes the same
// way it does. Answering that needs the reverse direction of the table, which
// is what this is — of whichever of the two tables the flags picked.
const std::vector<uint32_t>& caseCandidates(uint32_t cc, bool unicode);

}  // namespace bronze::regex
