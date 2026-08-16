#include "regex/chars.h"

#include <algorithm>
#include <map>

#include "regex/regex.h"
#include "regex/unicode.h"

namespace bronze::regex {

namespace {

// The three class-escape sets, built once. They are plain data and never
// mutated, so a function-local static is the whole of the lifetime story.
RangeList makeDigits() {
    RangeList list;
    addRange(list, '0', '9');
    return list;
}

// 22.2.2.9's WhiteSpace + LineTerminator, which is the same set
// `String.prototype.trim` strips — deliberately, so a program cannot find one
// definition of "space" in `trim` and another in `\s`.
RangeList makeSpaces() {
    RangeList list;
    addRange(list, 0x0009, 0x000D);
    addRange(list, 0x0020, 0x0020);
    addRange(list, 0x00A0, 0x00A0);
    addRange(list, 0x1680, 0x1680);
    addRange(list, 0x2000, 0x200A);
    addRange(list, 0x2028, 0x2029);
    addRange(list, 0x202F, 0x202F);
    addRange(list, 0x205F, 0x205F);
    addRange(list, 0x3000, 0x3000);
    addRange(list, 0xFEFF, 0xFEFF);
    normalizeRanges(list);
    return list;
}

// 22.2.2.7.1 WordCharacters: the basic sixty-three, plus every character that
// is not one of them whose Canonicalize IS one of them.
//
// The second set is DERIVED from `canonicalize` rather than written out, and
// that is the whole point of the function. A hard-coded member is a second
// opinion about the fold, and two opinions drift: the list here used to name
// U+017F and U+212A while `canonicalize` — correctly, for a pattern without
// `u` — returns both unchanged, so `/ſ/i.test("s")` answered false and
// `/\w/i.test("ſ")` answered true in the same program.
//
// Step 3 asserts the extra set is EMPTY unless both [[Unicode]] and
// [[IgnoreCase]] hold. Now that both can, it comes out empty in three of the
// four modes and holds exactly U+017F and U+212A in the fourth, where simple
// case folding maps them to `s` and `k` and the uppercase table keeps them.
// Neither answer is written down; both fall out of whichever table the flags
// picked, which is what makes the set a pattern builds and the comparison the
// matcher runs incapable of disagreeing.
//
// Under `u` the alphabet is the whole code space, and walking all 1114112 of
// it per mode would be paid at the first `\w` in a program. It walks the fold
// table's SOURCES instead: those are exactly the characters whose
// canonicalization is not themselves, so every character skipped canonicalizes
// to itself, and one that is not already basic cannot then be in `basic`.
// Same derivation, same answer — and `tests/regex` walks the whole alphabet
// against it, so "same answer" is checked rather than argued.
RangeList makeWords(bool ignoreCase, bool unicode) {
    RangeList basic;
    addRange(basic, '0', '9');
    addRange(basic, 'A', 'Z');
    addRange(basic, '_', '_');
    addRange(basic, 'a', 'z');
    normalizeRanges(basic);

    RangeList list = basic;
    const auto consider = [&](uint32_t code) {
        if (rangesContain(basic, code)) return;
        if (rangesContain(basic, canonicalize(code, ignoreCase, unicode))) {
            addRange(list, code, code);
        }
    };
    if (unicode) {
        for (uint32_t code : simpleCaseFoldSources()) consider(code);
    } else {
        for (uint32_t unit = 0; unit <= kMaxUnit; ++unit) consider(unit);
    }
    normalizeRanges(list);
    return list;
}

// Blocks whose members carry UPPERCASE mappings bronze has no table for, and
// which a pattern under `i` alone therefore cannot be answered about. Being
// generous here costs a hard error on a pattern that would have worked; being
// stingy costs a silent wrong answer, which is the trade
// hard-errors-over-silent-fallbacks already settled.
//
// It says nothing about `u` and `i` together: that mode folds from a generated
// UCD table with no holes in it, so every block below is answerable there and
// none of them is refused. The list is the shape of ONE of the two tables.
//
// The holes in this list are the blocks written out below: Latin Extended-A,
// Greek and Coptic, Cyrillic with its supplement, and the two Armenian letter
// runs. Everything still listed is refused because its mappings are irregular
// enough that stating them from memory would be a guess — Latin Extended-B in
// particular, whose uppercase mappings jump around the block and out of it.
constexpr Range kUnknownCasedBlocks[] = {
    {0x0180, 0x02FF},  // Latin Extended-B, IPA Extensions, modifier letters
    {0x0300, 0x036F},  // combining marks — U+0345 uppercases to U+0399
    // U+037A GREEK YPOGEGRAMMENI is the spacing compatibility form of U+0345,
    // and whether the Default Case Conversion gives it that character's
    // uppercase or leaves it alone is exactly the kind of question this list
    // exists to refuse rather than answer from memory.
    {0x037A, 0x037A},
    {0x0557, 0x0560},  // between the two Armenian letter runs
    {0x0587, 0x058F},  // ARMENIAN SMALL LIGATURE ECH YIWN and the symbols after
    {0x10A0, 0x10FF},  // Georgian
    {0x13A0, 0x13FF},  // Cherokee
    {0x1C80, 0x1CBF},  // Cyrillic Extended-C, Georgian Extended
    {0x1E00, 0x1FFF},  // Latin Extended Additional, Greek Extended
    {0x2100, 0x218F},  // letterlike symbols, Roman numerals
    {0x24B6, 0x24E9},  // circled letters
    {0x2C00, 0x2D2F},  // Glagolitic, Latin Extended-C, Coptic
    {0xA640, 0xA7FF},  // Cyrillic Extended-B, Latin Extended-D
    {0xAB53, 0xABBF},  // Cherokee small letters
    {0xFB00, 0xFB17},  // Latin and Armenian ligatures
    {0xFF21, 0xFF5A},  // fullwidth Latin
};

constexpr size_t kUnknownCasedBlockCount =
    sizeof(kUnknownCasedBlocks) / sizeof(kUnknownCasedBlocks[0]);

// The order is load-bearing, not cosmetic: a class range reports the LOWEST
// refused unit it contains, and the scan stops at the first block it overlaps.
// Written out ascending and disjoint above, and checked here so that adding a
// block in the wrong place is a compile error rather than a diagnostic naming
// the wrong code point.
constexpr bool unknownCasedBlocksAscend() {
    for (size_t i = 1; i < kUnknownCasedBlockCount; ++i) {
        if (kUnknownCasedBlocks[i - 1].hi >= kUnknownCasedBlocks[i].lo) return false;
    }
    return true;
}
static_assert(unknownCasedBlocksAscend(),
              "kUnknownCasedBlocks must be sorted and disjoint");

// ---- the case table, block by block ---------------------------------------
//
// Each function below is one contiguous run of the Unicode Default Case
// Conversion table, written as the rule that generated it plus the members
// that break the rule. That is the only form in which a table with no data
// file behind it can be checked: a reader can name the rule, and the
// exceptions are short enough to read one by one.

uint16_t foldLatin1(uint16_t unit) {
    if (unit >= 'a' && unit <= 'z') return static_cast<uint16_t>(unit - 32);
    // U+00DF's uppercase is "SS", two units, and 22.2.2.9 step 3 leaves a
    // multi-unit uppercase alone — which is why `/ß/i` does not match "SS".
    if (unit == 0x00DF) return unit;
    // U+00B5 MICRO SIGN uppercases to U+039C GREEK CAPITAL LETTER MU, and
    // U+00FF to U+0178: both leave Latin-1, and both are kept, because the
    // "uppercase escaped to ASCII" guard of step 4 only fires when the result
    // is BELOW 128.
    if (unit == 0x00B5) return 0x039C;
    if (unit == 0x00FF) return 0x0178;
    if (unit >= 0x00E0 && unit <= 0x00FE && unit != 0x00F7) {
        return static_cast<uint16_t>(unit - 32);
    }
    return unit;
}

// Latin Extended-A, U+0100..U+017F. Almost the whole block is accented Latin
// letters encoded as adjacent capital/small pairs; the block changes phase
// three times, so which of the pair is the capital depends on where you are.
uint16_t foldLatinExtendedA(uint16_t unit) {
    switch (unit) {
        // U+0130 LATIN CAPITAL LETTER I WITH DOT ABOVE is itself a capital, so
        // it is not the partner of the character after it.
        case 0x0130: return unit;
        // U+0131 LATIN SMALL LETTER DOTLESS I uppercases to ASCII `I`, and
        // step 4 keeps a non-ASCII character whose uppercase is ASCII — so
        // `/ı/i` does NOT match "I".
        case 0x0131: return unit;
        // U+0138 LATIN SMALL LETTER KRA has no uppercase at all.
        case 0x0138: return unit;
        // U+0149's uppercase is "ʼN", two units, so step 3 leaves it.
        case 0x0149: return unit;
        // U+0178 LATIN CAPITAL LETTER Y WITH DIAERESIS is a capital; it is the
        // uppercase of U+00FF, which is why it sits alone here.
        case 0x0178: return unit;
        // U+017F LATIN SMALL LETTER LONG S uppercases to ASCII `S`: step 4
        // again, so `/ſ/i` does not match "s" — and, through `makeWords`, `\w`
        // does not hold it either. Under `u` and `i` both answers flip, and
        // they flip together, because both come from `canonicalize`.
        case 0x017F: return unit;
        default: break;
    }
    // U+0100..U+0137: even is the capital.
    if (unit <= 0x0137) return (unit & 1) ? static_cast<uint16_t>(unit - 1) : unit;
    // U+0139..U+0148: the phase flips at U+0139 LATIN CAPITAL LETTER L WITH
    // ACUTE, so odd is the capital.
    if (unit <= 0x0148) return (unit & 1) ? unit : static_cast<uint16_t>(unit - 1);
    // U+014A..U+0177: back to even-is-capital at U+014A LATIN CAPITAL LETTER
    // ENG.
    if (unit <= 0x0177) return (unit & 1) ? static_cast<uint16_t>(unit - 1) : unit;
    // U+0179..U+017E: odd again, from LATIN CAPITAL LETTER Z WITH ACUTE.
    return (unit & 1) ? unit : static_cast<uint16_t>(unit - 1);
}

// Greek and Coptic, U+0370..U+03FF. The modern alphabet is a +32 offset
// (U+03B1 α .. U+03C9 ω against U+0391 Α .. U+03A9 Ω), and everything else in
// the block is either an accented vowel whose capital sits below U+0390, a
// glyph-variant symbol whose uppercase is the ordinary letter, or one of the
// archaic capital/small pairs at U+03D8..U+03EF.
uint16_t foldGreek(uint16_t unit) {
    // The offset run, split at U+03C2 GREEK SMALL LETTER FINAL SIGMA: its
    // uppercase is U+03A3, the SAME capital as U+03C3's, so it is -31 and not
    // -32 and the run cannot be written as one range.
    if (unit >= 0x03B1 && unit <= 0x03C1) return static_cast<uint16_t>(unit - 32);
    if (unit == 0x03C2) return 0x03A3;
    if (unit >= 0x03C3 && unit <= 0x03CB) return static_cast<uint16_t>(unit - 32);

    switch (unit) {
        // The tonos vowels: the capitals were encoded before the alphabet, at
        // U+0386..U+038F, so these are not an offset at all.
        case 0x03AC: return 0x0386;  // ά
        case 0x03AD: return 0x0388;  // έ
        case 0x03AE: return 0x0389;  // ή
        case 0x03AF: return 0x038A;  // ί
        case 0x03CC: return 0x038C;  // ό
        case 0x03CD: return 0x038E;  // ύ
        case 0x03CE: return 0x038F;  // ώ
        // U+0390 and U+03B0 (iota and upsilon with dialytika AND tonos) are
        // deliberately absent: their uppercase is three code units, so step 3
        // leaves them alone. Both fall through to the identity below.

        // The three letters added with their capitals immediately before them.
        case 0x0371: return 0x0370;  // ͱ heta
        case 0x0373: return 0x0372;  // ͳ archaic sampi
        case 0x0377: return 0x0376;  // ͷ pamphylian digamma
        // The lunate sigma variants, whose capitals are at the END of the
        // block rather than beside them.
        case 0x037B: return 0x03FD;  // ͻ reversed lunate sigma
        case 0x037C: return 0x03FE;  // ͼ dotted lunate sigma
        case 0x037D: return 0x03FF;  // ͽ reversed dotted lunate sigma
        // The glyph-variant symbols: each uppercases to the ordinary capital
        // of the letter it is a variant of, which is what makes `/[ϐ]/i` match
        // "Β" and is the reason these cannot be skipped.
        case 0x03D0: return 0x0392;  // ϐ beta symbol
        case 0x03D1: return 0x0398;  // ϑ theta symbol
        case 0x03D5: return 0x03A6;  // ϕ phi symbol
        case 0x03D6: return 0x03A0;  // ϖ pi symbol
        case 0x03D7: return 0x03CF;  // ϗ kai symbol
        case 0x03F0: return 0x039A;  // ϰ kappa symbol
        case 0x03F1: return 0x03A1;  // ϱ rho symbol
        case 0x03F2: return 0x03F9;  // ϲ lunate sigma symbol
        case 0x03F3: return 0x037F;  // ϳ yot
        case 0x03F5: return 0x0395;  // ϵ lunate epsilon symbol
        case 0x03F8: return 0x03F7;  // ϸ sho
        case 0x03FB: return 0x03FA;  // ϻ san
        // U+03FC GREEK RHO WITH STROKE SYMBOL has no uppercase; identity.
        default: break;
    }
    // The archaic Greek letters (archaic koppa, stigma, digamma, koppa, sampi)
    // and the Coptic letters kept in this block, all encoded capital first and
    // small immediately after: U+03D8/U+03D9 through U+03EE/U+03EF.
    if (unit >= 0x03D8 && unit <= 0x03EF && (unit & 1) != 0) {
        return static_cast<uint16_t>(unit - 1);
    }
    return unit;
}

// Cyrillic, U+0400..U+04FF, and the Cyrillic Supplement, U+0500..U+052F.
// Three regular runs and one exception.
uint16_t foldCyrillic(uint16_t unit) {
    // The main alphabet: А..Я at U+0410..U+042F against а..я at
    // U+0430..U+044F, a +32 offset exactly like ASCII's.
    if (unit >= 0x0430 && unit <= 0x044F) return static_cast<uint16_t>(unit - 32);
    // The extended-Cyrillic letters of the other Slavic alphabets were encoded
    // as a block of 16 capitals at U+0400 with their small letters at U+0450,
    // so the offset there is 0x50 and not 0x20.
    if (unit >= 0x0450 && unit <= 0x045F) return static_cast<uint16_t>(unit - 80);
    // U+04CF CYRILLIC SMALL LETTER PALOCHKA was added long after its capital
    // U+04C0, so it is the one member of the block not adjacent to its pair —
    // and it is what makes U+04C1..U+04CE odd-is-capital instead of even.
    if (unit == 0x04CF) return 0x04C0;
    if (unit >= 0x0460 && unit <= 0x0481 && (unit & 1) != 0) {
        return static_cast<uint16_t>(unit - 1);
    }
    // U+0482..U+0489 are a currency sign and the combining Cyrillic marks: no
    // case, and the reason the paired runs below start at U+048A.
    if (unit >= 0x048A && unit <= 0x04BF && (unit & 1) != 0) {
        return static_cast<uint16_t>(unit - 1);
    }
    if (unit >= 0x04C1 && unit <= 0x04CE && (unit & 1) == 0) {
        return static_cast<uint16_t>(unit - 1);
    }
    if (unit >= 0x04D0 && unit <= 0x052F && (unit & 1) != 0) {
        return static_cast<uint16_t>(unit - 1);
    }
    return unit;
}

// The uppercase table's reverse direction, over the units it has data for.
// Built once from `canonicalize` itself, so the two cannot drift. A unit that
// canonicalizes to itself is left out: the caller has already tested the
// canonicalization directly, so an identity entry would only make every lookup
// carry a member that can never decide anything.
//
// This is the `i`-without-`u` half only. The fold's reverse direction is
// generated with the fold, in `regex/unicode.cpp`, because building it by a
// walk over 1114112 code points would be the same table at a thousand times
// the cost.
const std::map<uint32_t, std::vector<uint32_t>>& reverseUppercaseTable() {
    static const std::map<uint32_t, std::vector<uint32_t>> table = [] {
        std::map<uint32_t, std::vector<uint32_t>> out;
        for (uint32_t u = 0; u <= kMaxUnit; ++u) {
            const uint32_t cc = canonicalize(u, /*ignoreCase=*/true, /*unicode=*/false);
            if (cc == u) continue;
            out[cc].push_back(u);
        }
        return out;
    }();
    return table;
}

}  // namespace

void addRange(RangeList& list, uint32_t lo, uint32_t hi) {
    if (lo > hi) return;
    list.push_back(Range{lo, hi});
}

void normalizeRanges(RangeList& list) {
    std::sort(list.begin(), list.end(),
              [](const Range& a, const Range& b) { return a.lo != b.lo ? a.lo < b.lo : a.hi < b.hi; });
    RangeList merged;
    for (const Range& r : list) {
        // `+ 1` merges ADJACENT ranges too, so [a-c][d-f] becomes one range.
        // Membership does not care, but the complement does: two ranges with
        // no gap between them would otherwise produce an empty gap range.
        if (!merged.empty() && r.lo <= merged.back().hi + 1) {
            merged.back().hi = std::max(merged.back().hi, r.hi);
        } else {
            merged.push_back(r);
        }
    }
    list.swap(merged);
}

RangeList complementRanges(const RangeList& list, uint32_t ceiling) {
    RangeList sorted = list;
    normalizeRanges(sorted);
    RangeList out;
    uint32_t next = 0;
    for (const Range& r : sorted) {
        // A member above the ceiling is outside the alphabet being complemented
        // and cannot punch a hole in it. Tested before the gap is emitted, so
        // the output can never name a code point the mode has no word for.
        if (r.lo > ceiling) break;
        if (r.lo > next) out.push_back(Range{next, r.lo - 1});
        if (r.hi >= ceiling) return out;
        next = r.hi + 1;
    }
    if (next <= ceiling) out.push_back(Range{next, ceiling});
    return out;
}

// Both are one walk over two normalized lists, taking the overlap of the two
// fronts and dropping whichever ends first. Written as a pair rather than as
// `a - b == a & ~b` because the complement needs a ceiling and these must not:
// a difference taken through a complement over the wrong alphabet would quietly
// hand back every code point above it.
RangeList intersectRanges(const RangeList& a, const RangeList& b) {
    RangeList left = a;
    RangeList right = b;
    normalizeRanges(left);
    normalizeRanges(right);
    RangeList out;
    size_t i = 0;
    size_t j = 0;
    while (i < left.size() && j < right.size()) {
        const uint32_t lo = std::max(left[i].lo, right[j].lo);
        const uint32_t hi = std::min(left[i].hi, right[j].hi);
        if (lo <= hi) out.push_back(Range{lo, hi});
        if (left[i].hi < right[j].hi) {
            ++i;
        } else {
            ++j;
        }
    }
    normalizeRanges(out);
    return out;
}

RangeList subtractRanges(const RangeList& a, const RangeList& b) {
    RangeList left = a;
    RangeList right = b;
    normalizeRanges(left);
    normalizeRanges(right);
    RangeList out;
    size_t j = 0;
    for (const Range& r : left) {
        uint32_t next = r.lo;
        // Every range of `right` that can overlap `r`, in order. `j` is never
        // rewound: `right` is sorted and disjoint, so a range that ended before
        // `r` began can never meet a later one.
        while (j < right.size() && right[j].hi < r.lo) ++j;
        for (size_t k = j; k < right.size() && right[k].lo <= r.hi; ++k) {
            if (right[k].lo > next) out.push_back(Range{next, right[k].lo - 1});
            if (right[k].hi >= next) next = right[k].hi + 1;
            if (next > r.hi) break;
        }
        if (next <= r.hi) out.push_back(Range{next, r.hi});
    }
    normalizeRanges(out);
    return out;
}

bool rangesContain(const RangeList& list, uint32_t code) {
    for (const Range& r : list) {
        if (code < r.lo) return false;  // normalized: sorted and disjoint
        if (code <= r.hi) return true;
    }
    return false;
}

namespace {

bool isLeadSurrogate(uint32_t unit) { return unit >= 0xD800 && unit <= 0xDBFF; }
bool isTrailSurrogate(uint32_t unit) { return unit >= 0xDC00 && unit <= 0xDFFF; }

uint32_t combineSurrogates(uint32_t lead, uint32_t trail) {
    return 0x10000 + ((lead - 0xD800) << 10) + (trail - 0xDC00);
}

}  // namespace

CodePointStep codePointAt(std::u16string_view input, size_t index, bool unicode) {
    const uint32_t first = input[index];
    if (!unicode || !isLeadSurrogate(first) || index + 1 >= input.size()) return {first, 1};
    const uint32_t second = input[index + 1];
    if (!isTrailSurrogate(second)) return {first, 1};
    return {combineSurrogates(first, second), 2};
}

CodePointStep codePointBefore(std::u16string_view input, size_t index, bool unicode) {
    const uint32_t last = input[index - 1];
    if (!unicode || !isTrailSurrogate(last) || index < 2) return {last, 1};
    const uint32_t first = input[index - 2];
    if (!isLeadSurrogate(first)) return {last, 1};
    return {combineSurrogates(first, last), 2};
}

// 22.2.7.3 AdvanceStringIndex. Every cursor a global match moves — `lastIndex`
// after an empty match, the scan in `search`, the step `split` takes past a
// separator that matched nothing — is this one operation, so a program cannot
// find one of them stepping by a unit and another by a character.
size_t advanceStringIndex(UnitsView input, size_t index, bool unicode) {
    if (index >= input.size()) return index + 1;
    return index + codePointAt(input, index, unicode).width;
}

const RangeList& digitRanges() {
    static const RangeList list = makeDigits();
    return list;
}

const RangeList& spaceRanges() {
    static const RangeList list = makeSpaces();
    return list;
}

const RangeList& wordRanges(bool ignoreCase, bool unicode) {
    static const RangeList plain = makeWords(false, false);
    static const RangeList folded = makeWords(true, false);
    static const RangeList plainUnicode = makeWords(false, true);
    static const RangeList foldedUnicode = makeWords(true, true);
    if (!unicode) return ignoreCase ? folded : plain;
    return ignoreCase ? foldedUnicode : plainUnicode;
}

uint32_t canonicalize(uint32_t code, bool ignoreCase, bool unicode) {
    if (!ignoreCase) return code;
    // 22.2.2.9 step 1: with BOTH flags the table is simple case folding, which
    // comes from the UCD and covers the whole code space — the one branch an
    // astral character can reach, and the one that answers for U+017F, U+212A
    // and every character above U+FFFF that has a case at all.
    if (unicode) return simpleCaseFold(code);
    // Step 3's toUppercase, over the blocks below. A code point above 0xFFFF
    // cannot arrive here: without `u` the alphabet IS the code unit, so
    // nothing a pattern or a subject can produce exceeds one.
    const uint16_t unit = static_cast<uint16_t>(code);
    if (unit < 0x0100) return foldLatin1(unit);
    if (unit < 0x0180) return foldLatinExtendedA(unit);
    if (unit >= 0x0370 && unit <= 0x03FF) return foldGreek(unit);
    if (unit >= 0x0400 && unit <= 0x052F) return foldCyrillic(unit);
    // Armenian: the small letters at U+0561..U+0586 against the capitals at
    // U+0531..U+0556, an offset of 0x30 because the capitals are a run of 38
    // letters and the small letters begin one row further on. U+0587 ARMENIAN
    // SMALL LIGATURE ECH YIWN is NOT part of it — its uppercase is two units —
    // and it is refused rather than folded.
    if (unit >= 0x0561 && unit <= 0x0586) return static_cast<uint16_t>(unit - 48);
    return unit;
}

bool firstUnknownCasedUnitInRange(uint32_t lo, uint32_t hi, uint32_t& out) {
    if (lo > hi) return false;
    for (const Range& r : kUnknownCasedBlocks) {
        if (r.lo > hi) return false;  // ascending: no later block can overlap
        if (r.hi < lo) continue;
        out = lo > r.lo ? lo : r.lo;
        return true;
    }
    return false;
}

// The one-unit case of the range question, and written as it so the two cannot
// answer differently about the same unit.
bool isUnknownCasedUnit(uint32_t unit) {
    uint32_t offender = 0;
    return firstUnknownCasedUnitInRange(unit, unit, offender);
}

const std::vector<uint32_t>& caseCandidates(uint32_t cc, bool unicode) {
    // Whichever table `canonicalize` used going forward, read backwards. The
    // two are never mixed: a candidate from the uppercase table would answer
    // about a fold the matcher is not performing.
    if (unicode) return simpleCaseFoldCandidates(cc);
    static const std::vector<uint32_t> none;
    const auto& table = reverseUppercaseTable();
    auto it = table.find(cc);
    return it == table.end() ? none : it->second;
}

}  // namespace bronze::regex
