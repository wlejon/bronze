// The runtime's NORMALIZATION data (UAX #15), which String.prototype.normalize
// (ECMA-262 22.1.3.16) applies: canonical combining classes, the one-level
// decomposition mappings, and the explicit composition exclusion list.
//
// What is emitted is the UCD's own shape, one level deep, and nothing derived
// from it at a second remove: the full (recursive) decomposition, the
// composition pair table and the rest of Full_Composition_Exclusion
// (singletons, non-starter decompositions) are all functions of these, and
// src/runtime/unicode_norm.cpp computes them rather than this file carrying a
// second copy of one fact. Hangul syllables are absent for the same reason --
// UAX #15 defines them arithmetically, and UnicodeData.txt lists them as one
// range row with no mapping at all.
//
// The case tables are gen_unicode_runtime.cpp; the regex module's are
// gen_unicode_regex.cpp.

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "gen_unicode_common.h"

namespace gen {

namespace {

constexpr uint32_t kHangulFirst = 0xAC00;

// One code point's row of UnicodeData.txt, the two fields normalization reads:
// field 3 (Canonical_Combining_Class) and field 5 (Decomposition_Type and
// Decomposition_Mapping).
struct NormRow {
    uint8_t ccc = 0;
    bool compat = false;
    std::vector<uint32_t> mapping;
};

// One record of an emitted decomposition stream: `count` consecutive code
// points from `cp`. A record of count 1 carries its mapping; a longer one is a
// RUN of single-code-point mappings whose targets are consecutive too, and
// carries the first target alone.
struct DecompRecord {
    uint32_t cp;
    uint32_t count;
    std::vector<uint32_t> mapping;
};

struct CccRun {
    uint32_t first;
    uint32_t last;
    uint8_t ccc;
};

std::map<uint32_t, NormRow> parseNormRows(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(1);
    }
    std::map<uint32_t, NormRow> rows;
    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        std::vector<std::string> fields = split(trimmed, ';');
        if (fields.size() < 6) {
            std::cerr << "error: short UnicodeData.txt row: " << line << "\n";
            std::exit(1);
        }
        uint32_t cp = 0;
        if (!parseHex(fields[0], cp)) continue;
        const std::string name = trim(fields[1]);
        const std::string cccText = trim(fields[3]);
        const std::string decomp = trim(fields[5]);
        // A `First>`/`Last>` range row (CJK, Hangul, Tangut, private use)
        // stands for code points that are all starters with no mapping in this
        // file -- Hangul's mapping is arithmetic -- and the generator checks
        // that rather than expanding the range into rows that say nothing.
        if (!name.empty() && name.front() == '<' &&
            (name.find(", First>") != std::string::npos ||
             name.find(", Last>") != std::string::npos)) {
            if (cccText != "0" || !decomp.empty()) {
                std::cerr << "error: a range row carries normalization data: " << line << "\n";
                std::exit(1);
            }
            continue;
        }
        NormRow row;
        const int ccc = std::atoi(cccText.c_str());
        if (ccc < 0 || ccc > 254) {
            std::cerr << "error: combining class out of range: " << line << "\n";
            std::exit(1);
        }
        row.ccc = static_cast<uint8_t>(ccc);
        std::string mapping = decomp;
        if (!mapping.empty() && mapping.front() == '<') {
            const auto close = mapping.find('>');
            if (close == std::string::npos) {
                std::cerr << "error: unreadable decomposition tag: " << line << "\n";
                std::exit(1);
            }
            row.compat = true;
            mapping = mapping.substr(close + 1);
        }
        std::istringstream words(mapping);
        std::string word;
        while (words >> word) {
            uint32_t to = 0;
            if (!parseHex(word, to) || to == 0 || to > kMaxCodePoint) {
                std::cerr << "error: unreadable decomposition mapping: " << line << "\n";
                std::exit(1);
            }
            row.mapping.push_back(to);
        }
        if (row.compat && row.mapping.empty()) {
            std::cerr << "error: a decomposition tag with no mapping: " << line << "\n";
            std::exit(1);
        }
        if (!row.compat && row.mapping.size() > 2) {
            std::cerr << "error: a canonical decomposition of more than two code points: "
                      << line << "\n";
            std::exit(1);
        }
        if (row.ccc != 0 || !row.mapping.empty()) rows[cp] = row;
    }
    return rows;
}

// CompositionExclusions.txt's DATA lines: the script-specific exclusions and
// the post-composition-version additions. Its singleton and non-starter
// sections are published commented out, because both are derivable -- and
// the runtime derives them, as fullCompositionExclusion below does.
std::set<uint32_t> parseExplicitExclusions(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(1);
    }
    std::set<uint32_t> out;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> fields;
        if (!dataFields(line, fields)) continue;
        uint32_t first = 0;
        uint32_t last = 0;
        if (!parseCodeRange(fields[0], first, last)) {
            std::cerr << "error: unreadable CompositionExclusions.txt line: " << line << "\n";
            std::exit(1);
        }
        for (uint32_t cp = first; cp <= last; ++cp) out.insert(cp);
    }
    return out;
}

uint8_t cccOf(const std::map<uint32_t, NormRow>& rows, uint32_t cp) {
    auto it = rows.find(cp);
    return it == rows.end() ? 0 : it->second.ccc;
}

// Full_Composition_Exclusion (UAX #15 section 5.1): the explicit list, plus
// every singleton canonical decomposition, plus every non-starter
// decomposition -- a code point that is not a starter, or whose decomposition
// does not begin with one. The runtime applies the same three clauses.
bool fullCompositionExclusion(const std::map<uint32_t, NormRow>& rows,
                              const std::set<uint32_t>& explicitList, uint32_t cp) {
    auto it = rows.find(cp);
    if (it == rows.end() || it->second.compat || it->second.mapping.empty()) return false;
    const NormRow& row = it->second;
    if (explicitList.count(cp) != 0) return true;
    if (row.mapping.size() == 1) return true;
    return row.ccc != 0 || cccOf(rows, row.mapping[0]) != 0;
}

std::vector<DecompRecord> buildRecords(const std::map<uint32_t, NormRow>& rows, bool compat) {
    std::vector<DecompRecord> out;
    for (const auto& [cp, row] : rows) {
        if (row.mapping.empty() || row.compat != compat) continue;
        // Joined to the previous record when it continues it in BOTH code
        // spaces: fullwidth forms, mathematical alphanumerics and circled
        // numbers are runs, and a run is one record.
        if (row.mapping.size() == 1 && !out.empty()) {
            DecompRecord& last = out.back();
            if (last.mapping.size() == 1 && last.cp + last.count == cp &&
                last.mapping[0] + last.count == row.mapping[0]) {
                ++last.count;
                continue;
            }
        }
        out.push_back({cp, 1, row.mapping});
    }
    return out;
}

std::vector<CccRun> buildCccRuns(const std::map<uint32_t, NormRow>& rows) {
    std::vector<CccRun> out;
    for (const auto& [cp, row] : rows) {
        if (row.ccc == 0) continue;
        if (!out.empty() && out.back().last + 1 == cp && out.back().ccc == row.ccc) {
            out.back().last = cp;
            continue;
        }
        out.push_back({cp, cp, row.ccc});
    }
    return out;
}

// The one-level mapping a record list gives `cp`, or empty.
std::vector<uint32_t> lookup(const std::vector<DecompRecord>& records, uint32_t cp) {
    for (const DecompRecord& r : records) {
        if (cp < r.cp || cp >= r.cp + r.count) continue;
        if (r.count == 1) return r.mapping;
        return {r.mapping[0] + (cp - r.cp)};
    }
    return {};
}

// The structural check, in the shape the case tables' is: facts a wrong field
// index, a wrong run join or a 16-bit table would get wrong.
//   00C5 -> 0041 030A canonical; 1E0A -> 0044 0307 canonical.
//   212B ANGSTROM SIGN -> 00C5, a canonical SINGLETON.
//   0958 DEVANAGARI QA, 1D15E (astral) and 2ADC are on the explicit list;
//   0344 is not -- it is excluded as a non-starter, which the runtime derives.
//   FB01 -> 0066 0069 compatibility only; FF76 -> 30AB and 2460 -> 0031 in
//   runs; 1D400 -> 0041, a run start in the astral planes.
//   FDFA -> eighteen code points, the longest mapping there is.
//   0301 ccc 230, 0323 ccc 220, 1D165 ccc 216.
void checkTables(const std::map<uint32_t, NormRow>& rows,
                 const std::vector<DecompRecord>& canonical,
                 const std::vector<DecompRecord>& compat, const std::set<uint32_t>& explicitList) {
    using V = std::vector<uint32_t>;
    const bool ok =
        lookup(canonical, 0x00C5) == V{0x0041, 0x030A} &&
        lookup(canonical, 0x1E0A) == V{0x0044, 0x0307} && lookup(canonical, 0x212B) == V{0x00C5} &&
        lookup(canonical, 0x1D15E) == V{0x1D157, 0x1D165} && explicitList.count(0x0958) == 1 &&
        explicitList.count(0x1D15E) == 1 && explicitList.count(0x2ADC) == 1 &&
        explicitList.count(0x0344) == 0 && fullCompositionExclusion(rows, explicitList, 0x0344) &&
        !fullCompositionExclusion(rows, explicitList, 0x00C5) &&
        lookup(compat, 0xFB01) == V{0x0066, 0x0069} && lookup(canonical, 0xFB01).empty() &&
        lookup(compat, 0xFF76) == V{0x30AB} && lookup(compat, 0x2460) == V{0x0031} &&
        lookup(compat, 0x1D400) == V{0x0041} && lookup(compat, 0xFDFA).size() == 18 &&
        cccOf(rows, 0x0301) == 230 && cccOf(rows, 0x0323) == 220 &&
        cccOf(rows, 0x1D165) == 216 && cccOf(rows, 0x0041) == 0;
    if (!ok) {
        std::cerr << "error: normalization table check failed in UnicodeData.txt / "
                     "CompositionExclusions.txt\n";
        std::exit(1);
    }
}

// The smallest code point at which a string can stop being already in a
// form: below it, every code point is a starter with no mapping of the kind
// the form applies (and, for the composed forms, is never excluded and never
// the second of a composable pair), so the form is the identity. These are
// what let normalize() answer with its receiver without decoding it.
struct QuickBounds {
    uint32_t nfd = kHangulFirst;
    uint32_t nfkd = kHangulFirst;
    uint32_t nfc = kHangulFirst;
    uint32_t nfkc = kHangulFirst;
};

QuickBounds quickBounds(const std::map<uint32_t, NormRow>& rows,
                        const std::set<uint32_t>& explicitList) {
    QuickBounds q;
    // The Hangul jamo that compose as a second element, which no table row
    // spells: V (U+1161..) after L, T (U+11A8..) after LV.
    q.nfc = std::min<uint32_t>(q.nfc, 0x1161);
    for (const auto& [cp, row] : rows) {
        const bool decomposes = !row.mapping.empty();
        if (row.ccc != 0 || (decomposes && !row.compat)) q.nfd = std::min(q.nfd, cp);
        if (row.ccc != 0 || decomposes) q.nfkd = std::min(q.nfkd, cp);
        if (row.ccc != 0 || fullCompositionExclusion(rows, explicitList, cp)) {
            q.nfc = std::min(q.nfc, cp);
        }
        if (decomposes && !row.compat && row.mapping.size() == 2 &&
            !fullCompositionExclusion(rows, explicitList, cp)) {
            q.nfc = std::min(q.nfc, row.mapping[1]);
        }
    }
    q.nfkc = std::min(q.nfc, q.nfkd);
    // A bound at or past the surrogates would let a lone surrogate (and the
    // astral code points a pair spells) through the code-unit scan unread.
    if (q.nfc >= 0xD800 || q.nfkd >= 0xD800) {
        std::cerr << "error: a quick-check bound reaches the surrogates\n";
        std::exit(1);
    }
    return q;
}

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", v);
    return buf;
}

// The record stream's numbers, in emission order: `step, n, mapping...`, or
// `step, 0, count, first target` for a run. `step` is the distance from the
// previous record's LAST code point (from 0 for the first record), so a block
// of consecutive code points costs one small number each rather than a key.
std::vector<std::string> streamWords(const std::vector<DecompRecord>& records) {
    std::vector<std::string> words;
    uint32_t prevLast = 0;
    for (const DecompRecord& r : records) {
        words.push_back(std::to_string(r.cp - prevLast));
        if (r.count > 1) {
            words.push_back("0");
            words.push_back(std::to_string(r.count));
            words.push_back(hex(r.mapping[0]));
        } else {
            words.push_back(std::to_string(r.mapping.size()));
            for (uint32_t cp : r.mapping) words.push_back(hex(cp));
        }
        prevLast = r.cp + r.count - 1;
    }
    return words;
}

std::string generateNormHeader(size_t ccc, size_t canonical, size_t compat, size_t exclusions,
                               const QuickBounds& q) {
    std::ostringstream ss;
    ss << kBanner << "\n";
    ss << "#pragma once\n\n";
    ss << "#include <cstdint>\n\n";
    ss << "namespace bronze::runtime::unicode {\n\n";
    ss << "// Unicode Normalization (UAX #15), which String.prototype.normalize\n";
    ss << "// (ECMA-262 22.1.3.16) applies. One level of each mapping, as UnicodeData.txt\n";
    ss << "// gives it: the full decomposition, the composition pairs and the derived\n";
    ss << "// part of Full_Composition_Exclusion are computed from these by\n";
    ss << "// runtime/unicode_norm.cpp, and Hangul is arithmetic and absent.\n\n";
    ss << "// A run of code points sharing one nonzero Canonical_Combining_Class,\n";
    ss << "// ascending and disjoint; a code point in no run is a starter (class 0).\n";
    ss << "struct CccRun {\n";
    ss << "    uint32_t first;\n";
    ss << "    uint32_t last;\n";
    ss << "    uint8_t ccc;\n";
    ss << "};\n\n";
    ss << "extern const CccRun kCccRuns[];\n";
    ss << "constexpr uint32_t kCccRunCount = " << ccc << ";\n\n";
    ss << "// A decomposition STREAM, one record per code point that has a mapping, in\n";
    ss << "// ascending order:\n";
    ss << "//   step, n, m1 .. mn       the code point maps to the n code points m1..mn\n";
    ss << "//   step, 0, count, t       `count` consecutive code points map to t, t+1, ...\n";
    ss << "// `step` is the distance from the previous record's last code point (from 0\n";
    ss << "// for the first). It is read once, into a searchable index, by\n";
    ss << "// runtime/unicode_norm.cpp. Canonical and compatibility mappings are separate\n";
    ss << "// streams: a code point has at most one mapping, and NFD reads only the first.\n";
    ss << "extern const uint32_t kCanonicalDecompStream[];\n";
    ss << "constexpr uint32_t kCanonicalDecompStreamSize = " << canonical << ";\n";
    ss << "extern const uint32_t kCompatDecompStream[];\n";
    ss << "constexpr uint32_t kCompatDecompStreamSize = " << compat << ";\n\n";
    ss << "// CompositionExclusions.txt's explicit list, ascending: the script-specific\n";
    ss << "// and post-composition-version exclusions. The rest of\n";
    ss << "// Full_Composition_Exclusion (singletons, non-starter decompositions) follows\n";
    ss << "// from the tables above.\n";
    ss << "extern const uint32_t kCompositionExclusions[];\n";
    ss << "constexpr uint32_t kCompositionExclusionCount = " << exclusions << ";\n\n";
    ss << "// Below each bound every code point is a starter the form leaves alone (for\n";
    ss << "// the composed forms: never excluded and never the second of a composable\n";
    ss << "// pair), so a string whose code units are all below it is already in that\n";
    ss << "// form. Every bound is below the surrogates.\n";
    ss << "constexpr uint32_t kNfdQuickBelow = " << hex(q.nfd) << ";\n";
    ss << "constexpr uint32_t kNfkdQuickBelow = " << hex(q.nfkd) << ";\n";
    ss << "constexpr uint32_t kNfcQuickBelow = " << hex(q.nfc) << ";\n";
    ss << "constexpr uint32_t kNfkcQuickBelow = " << hex(q.nfkc) << ";\n\n";
    ss << "}  // namespace bronze::runtime::unicode\n";
    return ss.str();
}

// Comma-separated words wrapped at 100 columns, with no space after a comma:
// the streams are the bulk of the file and a space a word is a sixth of it.
void emitWords(std::ostringstream& ss, const std::vector<std::string>& words) {
    std::string line = "    ";
    for (const std::string& w : words) {
        if (line.size() + w.size() + 1 > 100) {
            ss << line << "\n";
            line = "    ";
        }
        line += w + ",";
    }
    if (line.size() > 4) ss << line << "\n";
}

std::string generateNormCpp(const std::vector<CccRun>& ccc,
                            const std::vector<std::string>& canonical,
                            const std::vector<std::string>& compat,
                            const std::set<uint32_t>& exclusions) {
    std::ostringstream ss;
    ss << kBanner << "\n";
    ss << "#include \"runtime/unicode_norm_data.h\"\n\n";
    ss << "namespace bronze::runtime::unicode {\n\n";
    ss << "const CccRun kCccRuns[] = {\n";
    std::vector<std::string> runs;
    for (const CccRun& r : ccc) {
        runs.push_back("{" + hex(r.first) + "," + hex(r.last) + "," + std::to_string(r.ccc) + "}");
    }
    emitWords(ss, runs);
    ss << "};\n\n";
    ss << "const uint32_t kCanonicalDecompStream[] = {\n";
    emitWords(ss, canonical);
    ss << "};\n\n";
    ss << "const uint32_t kCompatDecompStream[] = {\n";
    emitWords(ss, compat);
    ss << "};\n\n";
    ss << "const uint32_t kCompositionExclusions[] = {\n";
    std::vector<std::string> excl;
    for (uint32_t cp : exclusions) excl.push_back(hex(cp));
    emitWords(ss, excl);
    ss << "};\n\n";
    ss << "}  // namespace bronze::runtime::unicode\n";
    return ss.str();
}

}  // namespace

void generateNormTables(const std::filesystem::path& ucdDir, const std::filesystem::path& outDir) {
    const std::map<uint32_t, NormRow> rows = parseNormRows(ucdDir / "UnicodeData.txt");
    const std::set<uint32_t> explicitList =
        parseExplicitExclusions(ucdDir / "CompositionExclusions.txt");
    for (uint32_t cp : explicitList) {
        auto it = rows.find(cp);
        if (it == rows.end() || it->second.compat || it->second.mapping.size() != 2) {
            std::cerr << "error: an explicit composition exclusion that is not a canonical "
                         "pair: "
                      << hex(cp) << "\n";
            std::exit(1);
        }
    }
    const std::vector<DecompRecord> canonical = buildRecords(rows, false);
    const std::vector<DecompRecord> compat = buildRecords(rows, true);
    const std::vector<CccRun> ccc = buildCccRuns(rows);
    checkTables(rows, canonical, compat, explicitList);
    const QuickBounds q = quickBounds(rows, explicitList);
    const std::vector<std::string> canonicalWords = streamWords(canonical);
    const std::vector<std::string> compatWords = streamWords(compat);

    std::cout << "Parsed " << ccc.size() << " combining class runs, " << canonical.size()
              << " canonical and " << compat.size() << " compatibility decomposition records, "
              << explicitList.size() << " explicit composition exclusions\n";

    writeFile(outDir / "runtime" / "unicode_norm_data.h",
              generateNormHeader(ccc.size(), canonicalWords.size(), compatWords.size(),
                                 explicitList.size(), q));
    writeFile(outDir / "runtime" / "unicode_norm_data.cpp",
              generateNormCpp(ccc, canonicalWords, compatWords, explicitList));
}

}  // namespace gen
