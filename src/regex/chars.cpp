#include "regex/chars.h"

#include <algorithm>
#include <map>

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

RangeList makeWords(bool ignoreCase) {
    RangeList list;
    addRange(list, '0', '9');
    addRange(list, 'A', 'Z');
    addRange(list, '_', '_');
    addRange(list, 'a', 'z');
    if (ignoreCase) {
        // 22.2.2.7.1 step 4: a unit whose canonicalization is already a word
        // character is one too. Without `u` that is exactly these two, and
        // they are here rather than in `canonicalize` because they are a fact
        // about the SET and not about the comparison.
        addRange(list, 0x017F, 0x017F);
        addRange(list, 0x212A, 0x212A);
    }
    normalizeRanges(list);
    return list;
}

// Blocks whose members carry case mappings bronze has no table for. Being
// generous here costs a hard error on a pattern that would have worked; being
// stingy costs a silent wrong answer, which is the trade
// hard-errors-over-silent-fallbacks already settled.
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
        // again. It is why `wordRanges` has to name it separately.
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

// Canonicalize's reverse direction over the units bronze has data for. Built
// once from `canonicalize` itself, so the two cannot drift. A unit that
// canonicalizes to itself is left out: the caller has already tested the
// canonicalization directly, so an identity entry would only make every
// lookup carry a member that can never decide anything.
const std::map<uint16_t, std::vector<uint16_t>>& reverseCaseTable() {
    static const std::map<uint16_t, std::vector<uint16_t>> table = [] {
        std::map<uint16_t, std::vector<uint16_t>> out;
        for (uint32_t u = 0; u <= kMaxUnit; ++u) {
            const uint16_t unit = static_cast<uint16_t>(u);
            const uint16_t cc = canonicalize(unit, true);
            if (cc == unit) continue;
            out[cc].push_back(unit);
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

RangeList complementRanges(const RangeList& list) {
    RangeList sorted = list;
    normalizeRanges(sorted);
    RangeList out;
    uint32_t next = 0;
    for (const Range& r : sorted) {
        if (r.lo > next) out.push_back(Range{next, r.lo - 1});
        next = r.hi + 1;
        if (next > kMaxUnit) return out;
    }
    if (next <= kMaxUnit) out.push_back(Range{next, kMaxUnit});
    return out;
}

bool rangesContain(const RangeList& list, uint32_t unit) {
    for (const Range& r : list) {
        if (unit < r.lo) return false;  // normalized: sorted and disjoint
        if (unit <= r.hi) return true;
    }
    return false;
}

const RangeList& digitRanges() {
    static const RangeList list = makeDigits();
    return list;
}

const RangeList& spaceRanges() {
    static const RangeList list = makeSpaces();
    return list;
}

const RangeList& wordRanges(bool ignoreCase) {
    static const RangeList plain = makeWords(false);
    static const RangeList folded = makeWords(true);
    return ignoreCase ? folded : plain;
}

uint16_t canonicalize(uint16_t unit, bool ignoreCase) {
    if (!ignoreCase) return unit;
    if (unit < 0x0100) return foldLatin1(unit);
    if (unit < 0x0180) return foldLatinExtendedA(unit);
    if (unit >= 0x0370 && unit <= 0x03FF) return foldGreek(unit);
    if (unit >= 0x0400 && unit <= 0x052F) return foldCyrillic(unit);
    // Armenian: ա..ֆ at U+0561..U+0586 against Ա..Ֆ at U+0531..U+0556, an
    // offset of 0x30 because the capitals are a run of 38 letters and the
    // small letters begin one row further on. U+0587 ARMENIAN SMALL LIGATURE
    // ECH YIWN is NOT part of it — its uppercase is two units — and it is
    // refused rather than folded.
    if (unit >= 0x0561 && unit <= 0x0586) return static_cast<uint16_t>(unit - 48);
    return unit;
}

bool isUnknownCasedUnit(uint32_t unit) {
    for (const Range& r : kUnknownCasedBlocks) {
        if (unit >= r.lo && unit <= r.hi) return true;
    }
    return false;
}

const std::vector<uint16_t>& caseCandidates(uint16_t cc) {
    static const std::vector<uint16_t> none;
    const auto& table = reverseCaseTable();
    auto it = table.find(cc);
    return it == table.end() ? none : it->second;
}

}  // namespace bronze::regex
