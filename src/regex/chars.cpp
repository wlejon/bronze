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
// stingy costs a silent wrong answer, which is the trade docs/0001 decision 8
// already settled.
constexpr Range kUnknownCasedBlocks[] = {
    {0x0100, 0x02FF},  // Latin Extended-A/B, IPA Extensions
    {0x0300, 0x052F},  // combining marks with case, Greek, Cyrillic
    {0x0531, 0x058F},  // Armenian
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

// Canonicalize's reverse direction over the units bronze has data for. Built
// once from `canonicalize` itself, so the two cannot drift.
const std::map<uint16_t, std::vector<uint16_t>>& reverseCaseTable() {
    static const std::map<uint16_t, std::vector<uint16_t>> table = [] {
        std::map<uint16_t, std::vector<uint16_t>> out;
        for (uint32_t u = 0; u <= 0xFF; ++u) {
            out[canonicalize(static_cast<uint16_t>(u), true)].push_back(
                static_cast<uint16_t>(u));
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
