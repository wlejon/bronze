// Default Case Conversion over the generated tables next door.
//
// The tables are data and this is the algorithm, and the split is the reason
// this file is small: everything that changes when the UCD version changes is
// in `unicode_case_data_*.cpp`, and everything that changes when ECMA-262
// changes is here. There is exactly one rule here that is not a table lookup —
// Final_Sigma — and it is here because it is CONTEXT: which of "σ" and "ς" a
// capital sigma lowercases to depends on what surrounds it, so no table keyed
// by code point can hold the answer.

#include "runtime/unicode_case.h"

#include <algorithm>

#include "runtime/unicode_case_data.h"

namespace bronze::runtime::unicode {

namespace {

constexpr uint32_t kSigmaCapital = 0x03A3;
constexpr uint32_t kSigmaFinal = 0x03C2;
constexpr uint32_t kSigmaSmall = 0x03C3;

bool isLeadSurrogate(uint16_t u) { return u >= 0xD800 && u <= 0xDBFF; }
bool isTrailSurrogate(uint16_t u) { return u >= 0xDC00 && u <= 0xDFFF; }

// 11.1.4 CodePointAt, as a whole-string decode. An unpaired surrogate becomes a
// code point equal to itself, which re-encodes to the same unit — so a string
// that is not well formed survives a case conversion unchanged in that place
// rather than being repaired or replaced.
std::vector<uint32_t> decode(const std::vector<uint16_t>& units) {
    std::vector<uint32_t> out;
    out.reserve(units.size());
    for (size_t i = 0; i < units.size(); ++i) {
        const uint16_t first = units[i];
        if (isLeadSurrogate(first) && i + 1 < units.size() && isTrailSurrogate(units[i + 1])) {
            out.push_back(0x10000 + ((static_cast<uint32_t>(first) - 0xD800) << 10) +
                          (static_cast<uint32_t>(units[i + 1]) - 0xDC00));
            ++i;
            continue;
        }
        out.push_back(first);
    }
    return out;
}

// 11.1.3 UTF16EncodeCodePoint.
void encodeInto(uint32_t cp, std::vector<uint16_t>& out) {
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<uint16_t>(cp));
        return;
    }
    const uint32_t rest = cp - 0x10000;
    out.push_back(static_cast<uint16_t>(0xD800 + (rest >> 10)));
    out.push_back(static_cast<uint16_t>(0xDC00 + (rest & 0x3FF)));
}

// The simple tables are sorted by `from` and hold only the code points the
// mapping moves, so a miss is the identity mapping rather than an error.
uint32_t simpleMap(const CaseEntry* table, uint32_t count, uint32_t cp) {
    const CaseEntry* end = table + count;
    const CaseEntry* it = std::lower_bound(
        table, end, cp, [](const CaseEntry& e, uint32_t key) { return e.from < key; });
    return (it != end && it->from == cp) ? it->to : cp;
}

const FullCaseEntry* fullMap(const FullCaseEntry* table, uint32_t count, uint32_t cp) {
    const FullCaseEntry* end = table + count;
    const FullCaseEntry* it = std::lower_bound(
        table, end, cp, [](const FullCaseEntry& e, uint32_t key) { return e.from < key; });
    return (it != end && it->from == cp) ? it : nullptr;
}

bool inRanges(const PropRange* table, uint32_t count, uint32_t cp) {
    const PropRange* end = table + count;
    const PropRange* it = std::upper_bound(
        table, end, cp, [](uint32_t key, const PropRange& r) { return key < r.first; });
    if (it == table) return false;
    --it;
    return cp <= it->last;
}

bool isCased(uint32_t cp) { return inRanges(kCasedRanges, kCasedRangeCount, cp); }
bool isCaseIgnorable(uint32_t cp) {
    return inRanges(kCaseIgnorableRanges, kCaseIgnorableRangeCount, cp);
}

// SpecialCasing.txt's Final_Sigma, quoted: "C is preceded by a sequence
// consisting of a cased letter and then zero or more case-ignorable characters,
// and C is not followed by a sequence consisting of zero or more case-ignorable
// characters and then a cased letter."
//
// Both halves are the same scan in opposite directions: skip case-ignorable
// characters, then ask whether what stopped the scan is cased. Case-ignorable
// is tested FIRST because the two sets overlap — a modifier letter such as
// U+02B0 is both — and the condition's grammar consumes the ignorable run
// before it looks for the cased letter.
bool finalSigma(const std::vector<uint32_t>& cps, size_t at) {
    bool casedBefore = false;
    for (size_t j = at; j-- > 0;) {
        if (isCaseIgnorable(cps[j])) continue;
        casedBefore = isCased(cps[j]);
        break;
    }
    if (!casedBefore) return false;
    for (size_t k = at + 1; k < cps.size(); ++k) {
        if (isCaseIgnorable(cps[k])) continue;
        return !isCased(cps[k]);
    }
    return true;
}

// True when every unit is ASCII, in which case the conversion is the 26-letter
// one. Kept as a path of its own because it is most of the string work a real
// program does and because it is the ONE case where the answer has to be
// bit-for-bit what it was before the tables landed.
bool allAscii(const std::vector<uint16_t>& units) {
    for (uint16_t u : units) {
        if (u >= 0x80) return false;
    }
    return true;
}

}  // namespace

std::vector<uint16_t> toUpperFull(const std::vector<uint16_t>& units) {
    std::vector<uint16_t> out;
    out.reserve(units.size());
    if (allAscii(units)) {
        for (uint16_t u : units) {
            out.push_back(u >= 'a' && u <= 'z' ? static_cast<uint16_t>(u - 32) : u);
        }
        return out;
    }
    const std::vector<uint32_t> cps = decode(units);
    for (uint32_t cp : cps) {
        // The full mapping SHADOWS the simple one: a code point with both is
        // one whose 1:1 mapping loses information, which is the whole reason
        // SpecialCasing.txt exists.
        if (const FullCaseEntry* full = fullMap(kFullUppercase, kFullUppercaseCount, cp)) {
            for (uint8_t i = 0; i < full->count; ++i) encodeInto(full->to[i], out);
            continue;
        }
        encodeInto(simpleMap(kSimpleUppercase, kSimpleUppercaseCount, cp), out);
    }
    return out;
}

std::vector<uint16_t> toLowerFull(const std::vector<uint16_t>& units) {
    std::vector<uint16_t> out;
    out.reserve(units.size());
    if (allAscii(units)) {
        for (uint16_t u : units) {
            out.push_back(u >= 'A' && u <= 'Z' ? static_cast<uint16_t>(u + 32) : u);
        }
        return out;
    }
    const std::vector<uint32_t> cps = decode(units);
    for (size_t i = 0; i < cps.size(); ++i) {
        const uint32_t cp = cps[i];
        // The context rule, ahead of every table: a capital sigma at the end of
        // a word is the final form. This is the only place a lowercase answer
        // depends on anything but the code point, and getting it wrong makes
        // "ΣΑΣ" read "σασ" — which is not a Greek word.
        if (cp == kSigmaCapital) {
            encodeInto(finalSigma(cps, i) ? kSigmaFinal : kSigmaSmall, out);
            continue;
        }
        if (const FullCaseEntry* full = fullMap(kFullLowercase, kFullLowercaseCount, cp)) {
            for (uint8_t j = 0; j < full->count; ++j) encodeInto(full->to[j], out);
            continue;
        }
        encodeInto(simpleMap(kSimpleLowercase, kSimpleLowercaseCount, cp), out);
    }
    return out;
}

}  // namespace bronze::runtime::unicode
