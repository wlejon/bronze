// Unicode Normalization (UAX #15) over the generated tables next door.
//
// The tables carry ONE level of each mapping, as UnicodeData.txt does, and the
// explicit composition exclusions as CompositionExclusions.txt does; this file
// derives everything else from them once, on first use: a searchable index of
// each decomposition stream, and the composition pair table (every canonical
// pair that is not Full_Composition_Exclusion). Deriving rather than generating
// is what keeps one fact in one place -- a composition table emitted beside the
// decompositions could disagree with them, and this one cannot.
//
// Hangul syllables are arithmetic (UAX #15 section 3.12, "Hangul Syllable
// Decomposition / Composition") and never touch a table.

#include "runtime/unicode_norm.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "runtime/unicode_norm_data.h"

namespace bronze::runtime::unicode {

namespace {

constexpr uint32_t kSBase = 0xAC00;
constexpr uint32_t kLBase = 0x1100;
constexpr uint32_t kVBase = 0x1161;
constexpr uint32_t kTBase = 0x11A7;
constexpr uint32_t kLCount = 19;
constexpr uint32_t kVCount = 21;
constexpr uint32_t kTCount = 28;
constexpr uint32_t kNCount = kVCount * kTCount;
constexpr uint32_t kSCount = kLCount * kNCount;

// One record of a decomposition stream, located: `count` code points from
// `cp`. `length` is the mapping's length, or 0 for a run, whose code points map
// to stream[offset] plus their distance from `cp`.
struct DecompIndex {
    uint32_t cp;
    uint32_t count;
    uint32_t offset;
    uint32_t length;
};

struct NormTables {
    std::vector<DecompIndex> canonical;
    std::vector<DecompIndex> compat;
    // (first << 21 | second) -> the primary composite, ascending by key.
    std::vector<std::pair<uint64_t, uint32_t>> compose;
};

std::vector<DecompIndex> indexStream(const uint32_t* stream, uint32_t size) {
    std::vector<DecompIndex> out;
    uint32_t pos = 0;
    uint32_t prevLast = 0;
    while (pos < size) {
        const uint32_t cp = prevLast + stream[pos++];
        const uint32_t n = stream[pos++];
        DecompIndex entry{cp, 1, 0, n};
        if (n == 0) entry.count = stream[pos++];
        entry.offset = pos;
        pos += n == 0 ? 1 : n;
        out.push_back(entry);
        prevLast = cp + entry.count - 1;
    }
    // The stream's grammar ends exactly at its end, or the table and the
    // reader disagree about its shape -- which no answer could survive.
    if (pos != size) std::abort();
    return out;
}

bool explicitlyExcluded(uint32_t cp) {
    const uint32_t* end = kCompositionExclusions + kCompositionExclusionCount;
    return std::binary_search(kCompositionExclusions, end, cp);
}

NormTables buildTables() {
    NormTables t;
    t.canonical = indexStream(kCanonicalDecompStream, kCanonicalDecompStreamSize);
    t.compat = indexStream(kCompatDecompStream, kCompatDecompStreamSize);
    // A primary composite is a canonical PAIR mapping that is not
    // Full_Composition_Exclusion. A singleton never composes (it is a pair's
    // only shape that can), so what remains to exclude is the explicit list and
    // the non-starter decompositions: a code point that is not a starter, or
    // whose decomposition does not begin with one.
    for (const DecompIndex& e : t.canonical) {
        if (e.length != 2) continue;
        const uint32_t first = kCanonicalDecompStream[e.offset];
        const uint32_t second = kCanonicalDecompStream[e.offset + 1];
        if (explicitlyExcluded(e.cp) || combiningClass(e.cp) != 0 ||
            combiningClass(first) != 0) {
            continue;
        }
        t.compose.emplace_back((static_cast<uint64_t>(first) << 21) | second, e.cp);
    }
    std::sort(t.compose.begin(), t.compose.end());
    return t;
}

const NormTables& tables() {
    static const NormTables t = buildTables();
    return t;
}

const DecompIndex* findDecomp(const std::vector<DecompIndex>& index, uint32_t cp) {
    auto it = std::upper_bound(index.begin(), index.end(), cp,
                               [](uint32_t key, const DecompIndex& e) { return key < e.cp; });
    if (it == index.begin()) return nullptr;
    --it;
    return cp < it->cp + it->count ? &*it : nullptr;
}

bool isLeadSurrogate(uint16_t u) { return u >= 0xD800 && u <= 0xDBFF; }
bool isTrailSurrogate(uint16_t u) { return u >= 0xDC00 && u <= 0xDFFF; }

void encodeInto(uint32_t cp, std::vector<uint16_t>& out) {
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<uint16_t>(cp));
        return;
    }
    const uint32_t rest = cp - 0x10000;
    out.push_back(static_cast<uint16_t>(0xD800 + (rest >> 10)));
    out.push_back(static_cast<uint16_t>(0xDC00 + (rest & 0x3FF)));
}

// The full decomposition of one code point, appended: the mapping applied
// recursively until nothing in it decomposes further. A compatibility mapping
// is applied only under `compat`, and a canonical one always.
void decomposeInto(const NormTables& t, uint32_t cp, bool compat, std::vector<uint32_t>& out) {
    if (cp < kNfkdQuickBelow) {
        out.push_back(cp);
        return;
    }
    if (cp - kSBase < kSCount) {
        const uint32_t s = cp - kSBase;
        out.push_back(kLBase + s / kNCount);
        out.push_back(kVBase + (s % kNCount) / kTCount);
        if (s % kTCount != 0) out.push_back(kTBase + s % kTCount);
        return;
    }
    const uint32_t* stream = kCanonicalDecompStream;
    const DecompIndex* e = findDecomp(t.canonical, cp);
    if (e == nullptr && compat) {
        stream = kCompatDecompStream;
        e = findDecomp(t.compat, cp);
    }
    if (e == nullptr) {
        out.push_back(cp);
        return;
    }
    if (e->length == 0) {
        decomposeInto(t, stream[e->offset] + (cp - e->cp), compat, out);
        return;
    }
    for (uint32_t i = 0; i < e->length; ++i) {
        decomposeInto(t, stream[e->offset + i], compat, out);
    }
}

// Canonical Ordering (UAX #15 section 3.11 / Unicode 3.11 D109): within each
// run of non-starters, a stable sort by combining class. Runs are short, so an
// insertion sort that never moves a code point past an equal class.
void canonicalOrder(std::vector<uint32_t>& cps) {
    for (size_t i = 1; i < cps.size(); ++i) {
        const uint8_t cc = combiningClass(cps[i]);
        if (cc == 0) continue;
        size_t j = i;
        while (j > 0) {
            const uint8_t prev = combiningClass(cps[j - 1]);
            if (prev == 0 || prev <= cc) break;
            std::swap(cps[j], cps[j - 1]);
            --j;
        }
    }
}

// The primary composite of (first, second), or 0 when there is none.
uint32_t composePair(const NormTables& t, uint32_t first, uint32_t second) {
    if (first - kLBase < kLCount && second - kVBase < kVCount) {
        return kSBase + ((first - kLBase) * kVCount + (second - kVBase)) * kTCount;
    }
    if (first - kSBase < kSCount && (first - kSBase) % kTCount == 0 && second > kTBase &&
        second - kTBase < kTCount) {
        return first + (second - kTBase);
    }
    const uint64_t key = (static_cast<uint64_t>(first) << 21) | second;
    auto it = std::lower_bound(
        t.compose.begin(), t.compose.end(), key,
        [](const std::pair<uint64_t, uint32_t>& e, uint64_t k) { return e.first < k; });
    return (it != t.compose.end() && it->first == key) ? it->second : 0;
}

// Canonical Composition (UAX #15 section 3.11, D117): each character C, from
// the second on, is combined with the last starter L before it when C is not
// BLOCKED from L -- when nothing between them is a starter or has a combining
// class at least C's -- and L and C have a primary composite.
void canonicalCompose(const NormTables& t, std::vector<uint32_t>& cps) {
    if (cps.empty()) return;
    size_t starter = SIZE_MAX;
    uint8_t lastClass = 0;
    size_t write = 0;
    for (size_t read = 0; read < cps.size(); ++read) {
        const uint32_t c = cps[read];
        const uint8_t cc = combiningClass(c);
        if (starter != SIZE_MAX) {
            const bool adjacent = write == starter + 1;
            if (adjacent || (lastClass != 0 && lastClass < cc)) {
                const uint32_t composite = composePair(t, cps[starter], c);
                if (composite != 0) {
                    cps[starter] = composite;
                    continue;
                }
            }
        }
        if (cc == 0) starter = write;
        lastClass = cc;
        cps[write++] = c;
    }
    cps.resize(write);
}

uint32_t quickBound(NormForm form) {
    switch (form) {
        case NormForm::NFC: return kNfcQuickBelow;
        case NormForm::NFD: return kNfdQuickBelow;
        case NormForm::NFKC: return kNfkcQuickBelow;
        case NormForm::NFKD: return kNfkdQuickBelow;
    }
    return 0;
}

}  // namespace

uint8_t combiningClass(uint32_t cp) {
    if (cp < 0x300) return 0;
    const CccRun* end = kCccRuns + kCccRunCount;
    const CccRun* it = std::upper_bound(kCccRuns, end, cp,
                                        [](uint32_t key, const CccRun& r) { return key < r.first; });
    if (it == kCccRuns) return 0;
    --it;
    return cp <= it->last ? it->ccc : 0;
}

bool isTriviallyNormalized(const uint16_t* units, size_t length, NormForm form) {
    const uint32_t bound = quickBound(form);
    for (size_t i = 0; i < length; ++i) {
        if (units[i] >= bound) return false;
    }
    return true;
}

bool isTriviallyNormalizedLatin1(const char* bytes, size_t length, NormForm form) {
    const uint32_t bound = quickBound(form);
    if (bound > 0xFF) return true;
    for (size_t i = 0; i < length; ++i) {
        if (static_cast<unsigned char>(bytes[i]) >= bound) return false;
    }
    return true;
}

std::vector<uint16_t> normalize(const std::vector<uint16_t>& units, NormForm form) {
    const NormTables& t = tables();
    const bool compat = form == NormForm::NFKC || form == NormForm::NFKD;
    std::vector<uint32_t> cps;
    cps.reserve(units.size());
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t cp = units[i];
        if (isLeadSurrogate(units[i]) && i + 1 < units.size() && isTrailSurrogate(units[i + 1])) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(units[i + 1]) - 0xDC00);
            ++i;
        }
        decomposeInto(t, cp, compat, cps);
    }
    canonicalOrder(cps);
    if (form == NormForm::NFC || form == NormForm::NFKC) canonicalCompose(t, cps);
    std::vector<uint16_t> out;
    out.reserve(cps.size());
    for (uint32_t cp : cps) encodeInto(cp, out);
    return out;
}

}  // namespace bronze::runtime::unicode
