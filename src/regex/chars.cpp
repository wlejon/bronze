#include "regex/chars.h"

#include <algorithm>
#include <map>

#include "regex/regex.h"
#include "regex/unicode.h"

namespace bronze::regex {

namespace data {
// Defined, with this same layout, in the generated unicode_data_upper.cpp.
struct UnitMapping {
    uint16_t from;
    uint16_t to;
};
extern const UnitMapping kUppercaseUnits[];
extern const uint32_t kUppercaseUnitCount;
}  // namespace data

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
    // Steps 3 and 4: toUppercase of the one unit, by the generated table
    // (regex/unicode_data_upper.cpp). A code point above 0xFFFF cannot arrive
    // here: without `u` the alphabet IS the code unit.
    if (code < 0x80) return code >= 'a' && code <= 'z' ? code - 32 : code;
    const data::UnitMapping* begin = data::kUppercaseUnits;
    const data::UnitMapping* end = begin + data::kUppercaseUnitCount;
    const data::UnitMapping* it = std::lower_bound(
        begin, end, code, [](const data::UnitMapping& m, uint32_t c) { return m.from < c; });
    return it != end && it->from == code ? it->to : code;
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
