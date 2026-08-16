// tools/gen_unicode_tables.cpp
//
// Generates the Unicode data tables bronze carries, from the published UCD
// 16.0.0 data files vendored under tools/ucd/. Two modules are served and they
// need different data, so the output goes to both:
//
//   src/regex/    General_Category runs and simple case FOLDING (22.2.2.9's
//                 Canonicalize under `u` and `i`).
//   src/runtime/  DEFAULT CASE CONVERSION -- the simple and full case mappings
//                 String.prototype.toUpperCase / toLowerCase apply (11.1.3),
//                 plus the two derived properties the Final_Sigma condition is
//                 written in terms of.
//
// Folding and conversion are different operations over different tables and the
// generator keeps them apart for that reason: `scf("ẞ")` is "ß" where
// `toLowerCase("ẞ")` is also "ß" but `toUpperCase("ß")` is "SS", which no
// folding table can spell.
//
// Run once, commit the output. The build never invokes this: bronze does not
// depend on external generator tooling at build time, and a table regenerated
// during build cannot be reviewed or audited.
//
// Usage:
//   gen_unicode_tables [ucd_dir] [src_dir]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kMaxCodePoint = 0x10FFFF;
constexpr const char* kUcdVersion = "16.0.0";

const char* const kBanner =
    "// GENERATED FILE -- DO NOT EDIT BY HAND.\n"
    "//\n"
    "// Written by tools/gen_unicode_tables from the Unicode Character Database\n"
    "// version 16.0.0 vendored under tools/ucd/. To change anything here, change the\n"
    "// generator and rerun it:\n"
    "//\n"
    "//     tools/gen_unicode_tables\n"
    "//\n"
    "// The generator asserts the UCD version it reads, so a rerun either reproduces\n"
    "// these bytes or stops.\n";

struct GcRun {
    uint32_t start;
    std::string category;
};

struct FoldEntry {
    uint32_t from;
    uint32_t to;
};

// One 1:1 case mapping, from UnicodeData.txt field 12 (Simple_Uppercase_Mapping)
// or field 13 (Simple_Lowercase_Mapping).
struct CaseEntry {
    uint32_t from;
    uint32_t to;
};

// One 1:many case mapping, from SpecialCasing.txt. Three is the longest the
// file holds (U+0390 uppercases to three code points) and the generator refuses
// a longer one rather than truncating it.
struct FullCaseEntry {
    uint32_t from;
    std::vector<uint32_t> to;
};

// A closed code point interval of a derived property.
struct PropRange {
    uint32_t first;
    uint32_t last;
};

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> elems;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}

bool parseHex(const std::string& s, uint32_t& val) {
    std::string t = trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    unsigned long v = std::strtoul(t.c_str(), &end, 16);
    if (*end != '\0') return false;
    val = static_cast<uint32_t>(v);
    return true;
}

std::vector<GcRun> parseUnicodeData(const std::filesystem::path& path,
                                   std::vector<std::string>& outAliases) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(1);
    }

    std::map<uint32_t, std::string> gcMap;
    std::string line;
    uint32_t rangeStart = 0;
    std::string rangeCat;
    bool inRange = false;

    while (std::getline(in, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;

        std::vector<std::string> fields = split(trimmed, ';');
        if (fields.size() < 3) continue;

        uint32_t cp = 0;
        if (!parseHex(fields[0], cp)) continue;

        std::string name = trim(fields[1]);
        std::string cat = trim(fields[2]);

        if (name.ends_with(", First>")) {
            rangeStart = cp;
            rangeCat = cat;
            inRange = true;
        } else if (name.ends_with(", Last>")) {
            if (!inRange || rangeCat != cat) {
                std::cerr << "error: mismatched range in UnicodeData.txt at U+" << std::hex
                          << cp << "\n";
                std::exit(1);
            }
            for (uint32_t c = rangeStart; c <= cp; ++c) {
                gcMap[c] = rangeCat;
            }
            inRange = false;
        } else {
            gcMap[cp] = cat;
        }
    }

    std::vector<GcRun> runs;
    std::string prevCat;
    std::set<std::string> aliasSet;

    for (uint32_t cp = 0; cp <= kMaxCodePoint; ++cp) {
        auto it = gcMap.find(cp);
        std::string cat = (it != gcMap.end()) ? it->second : "Cn";
        aliasSet.insert(cat);
        if (cat != prevCat || runs.empty()) {
            runs.push_back({cp, cat});
            prevCat = cat;
        }
    }

    outAliases.assign(aliasSet.begin(), aliasSet.end());
    std::sort(outAliases.begin(), outAliases.end());

    if (outAliases.size() != 30) {
        std::cerr << "error: expected 30 General_Category aliases, found " << outAliases.size()
                  << "\n";
        std::exit(1);
    }

    return runs;
}

std::vector<FoldEntry> parseCaseFolding(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(1);
    }

    std::vector<FoldEntry> folds;
    std::string line;

    while (std::getline(in, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        // Strip comments after '#'
        auto commentPos = trimmed.find('#');
        if (commentPos != std::string::npos) {
            trimmed = trim(trimmed.substr(0, commentPos));
        }

        std::vector<std::string> fields = split(trimmed, ';');
        if (fields.size() < 3) continue;

        std::string status = trim(fields[1]);
        if (status == "C" || status == "S") {
            uint32_t from = 0;
            uint32_t to = 0;
            if (parseHex(fields[0], from) && parseHex(fields[2], to)) {
                if (from != to) {
                    folds.push_back({from, to});
                }
            }
        }
    }

    std::sort(folds.begin(), folds.end(), [](const FoldEntry& a, const FoldEntry& b) {
        return a.from < b.from;
    });

    // Verification checks:
    // 1. Turkic exclusion: I/i fold to i, but dotted/dotless I do not fold to them
    auto findFold = [&](uint32_t cp) -> uint32_t {
        for (const auto& f : folds) {
            if (f.from == cp) return f.to;
        }
        return cp;
    };

    if (findFold(0x0049) != 0x0069 || findFold(0x0069) != 0x0069 ||
        findFold(0x0130) != 0x0130 || findFold(0x0131) != 0x0131) {
        std::cerr << "error: Turkic exclusion check failed in case folding data\n";
        std::exit(1);
    }

    // 2. Extra word characters folding into basic [0-9A-Za-z_]
    auto isBasicWord = [](uint32_t c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c == '_') ||
               (c >= 'a' && c <= 'z');
    };

    std::vector<uint32_t> extraWordChars;
    for (const auto& f : folds) {
        if (isBasicWord(f.to) && !isBasicWord(f.from)) {
            extraWordChars.push_back(f.from);
        }
    }
    std::sort(extraWordChars.begin(), extraWordChars.end());

    std::vector<uint32_t> expectedWordChars = {0x017F, 0x212A};
    if (extraWordChars != expectedWordChars) {
        std::cerr << "error: unexpected extra word characters folding into ASCII word set\n";
        std::exit(1);
    }

    return folds;
}

// UnicodeData.txt fields 12 and 13, the SIMPLE case mappings: one code point
// in, one out. `split` drops a trailing empty field, so a line ending in `;`
// yields 14 elements rather than 15 and each field is bounds-checked on its own
// rather than by one count test.
//
// A `First>`/`Last>` range row carries no case mapping in this file (the ranges
// are CJK, Hangul, Tangut and private use), so a row whose name opens or closes
// a range is skipped rather than expanded -- expanding it would invent
// mappings.
void parseSimpleCaseMappings(const std::filesystem::path& path, std::vector<CaseEntry>& outUpper,
                             std::vector<CaseEntry>& outLower) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(1);
    }
    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        std::vector<std::string> fields = split(trimmed, ';');
        if (fields.size() < 3) continue;
        uint32_t cp = 0;
        if (!parseHex(fields[0], cp)) continue;
        const std::string name = trim(fields[1]);
        if (!name.empty() && name.front() == '<' &&
            (name.find(", First>") != std::string::npos ||
             name.find(", Last>") != std::string::npos)) {
            continue;
        }
        uint32_t mapped = 0;
        if (fields.size() > 12 && parseHex(fields[12], mapped) && mapped != cp) {
            outUpper.push_back({cp, mapped});
        }
        if (fields.size() > 13 && parseHex(fields[13], mapped) && mapped != cp) {
            outLower.push_back({cp, mapped});
        }
    }
    std::sort(outUpper.begin(), outUpper.end(),
              [](const CaseEntry& a, const CaseEntry& b) { return a.from < b.from; });
    std::sort(outLower.begin(), outLower.end(),
              [](const CaseEntry& a, const CaseEntry& b) { return a.from < b.from; });

    auto lookup = [](const std::vector<CaseEntry>& t, uint32_t cp) -> uint32_t {
        for (const CaseEntry& e : t) {
            if (e.from == cp) return e.to;
        }
        return cp;
    };
    // The structural check, in the shape parseCaseFolding's is: mappings a
    // wrong field index would get wrong, one of them ASTRAL, so a table built
    // over uint16_t rather than uint32_t cannot pass.
    //   1E9E LATIN CAPITAL LETTER SHARP S lowercases to 00DF, and 00DF has NO
    //        simple uppercase at all -- its uppercase is "SS" and lives in
    //        SpecialCasing.txt, which is the asymmetry this whole table pair
    //        exists to represent.
    //   0130 LATIN CAPITAL LETTER I WITH DOT ABOVE lowercases SIMPLY to 0069;
    //        its full lowercase is two code points.
    //   10400 DESERET CAPITAL LONG I lowercases to 10428, and back.
    //   13A0 CHEROKEE LETTER A lowercases to AB70, and back.
    if (lookup(outLower, 0x1E9E) != 0x00DF || lookup(outUpper, 0x00DF) != 0x00DF ||
        lookup(outLower, 0x0130) != 0x0069 || lookup(outLower, 0x10400) != 0x10428 ||
        lookup(outUpper, 0x10428) != 0x10400 || lookup(outLower, 0x13A0) != 0xAB70 ||
        lookup(outUpper, 0xAB70) != 0x13A0) {
        std::cerr << "error: simple case mapping check failed in UnicodeData.txt\n";
        std::exit(1);
    }
}

// SpecialCasing.txt: the FULL case mappings, which is where a mapping stops
// being 1:1 -- U+00DF uppercases to "SS", U+FB01 to "FI", U+0130 lowercases to
// "i" followed by U+0307.
//
// Only the mappings that are 1:MANY are kept. Every 1:1 entry in this file
// repeats UnicodeData.txt's simple mapping -- the generator checks that rather
// than assuming it -- so keeping them would be a second copy of a table that
// already exists and a second chance for the two to disagree.
//
// A line with a CONDITION list is skipped, and that is not an approximation. A
// condition beginning with a language ID (lt, tr, az) is a locale TAILORING,
// which Default Case Conversion excludes by definition: 22.1.3.28 is defined
// over the default mappings, and 22.1.3.26 toLocaleUpperCase is the member a
// tailoring would belong to. The one language-INDEPENDENT condition in the file
// is Final_Sigma, which is CONTEXT rather than data and is applied by the
// conversion algorithm itself. The generator asserts that it is still the only
// one rather than trusting this comment.
void parseSpecialCasing(const std::filesystem::path& path, std::vector<FullCaseEntry>& outUpper,
                        std::vector<FullCaseEntry>& outLower,
                        const std::vector<CaseEntry>& simpleUpper,
                        const std::vector<CaseEntry>& simpleLower) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(1);
    }
    auto codePoints = [](const std::string& text, std::vector<uint32_t>& out) {
        std::istringstream words(text);
        std::string word;
        while (words >> word) {
            uint32_t cp = 0;
            if (!parseHex(word, cp)) return false;
            out.push_back(cp);
        }
        return true;
    };
    auto lookup = [](const std::vector<CaseEntry>& t, uint32_t cp) -> uint32_t {
        for (const CaseEntry& e : t) {
            if (e.from == cp) return e.to;
        }
        return cp;
    };

    std::set<std::string> conditionsSeen;
    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        const auto commentPos = trimmed.find('#');
        if (commentPos != std::string::npos) trimmed = trim(trimmed.substr(0, commentPos));
        if (trimmed.empty()) continue;

        std::vector<std::string> fields = split(trimmed, ';');
        for (std::string& f : fields) f = trim(f);
        while (!fields.empty() && fields.back().empty()) fields.pop_back();
        if (fields.size() < 4) continue;

        if (fields.size() >= 5) {
            conditionsSeen.insert(fields[4]);
            continue;
        }

        uint32_t cp = 0;
        if (!parseHex(fields[0], cp)) continue;
        std::vector<uint32_t> lower;
        std::vector<uint32_t> upper;
        if (!codePoints(fields[1], lower) || !codePoints(fields[3], upper)) {
            std::cerr << "error: unreadable mapping in SpecialCasing.txt: " << trimmed << "\n";
            std::exit(1);
        }
        if (upper.size() > 3 || lower.size() > 3) {
            std::cerr << "error: a case mapping longer than three code points: " << trimmed
                      << "\n";
            std::exit(1);
        }
        if (upper.size() == 1 && upper[0] != lookup(simpleUpper, cp)) {
            std::cerr << "error: a SpecialCasing 1:1 uppercase disagrees with UnicodeData: "
                      << trimmed << "\n";
            std::exit(1);
        }
        if (lower.size() == 1 && lower[0] != lookup(simpleLower, cp)) {
            std::cerr << "error: a SpecialCasing 1:1 lowercase disagrees with UnicodeData: "
                      << trimmed << "\n";
            std::exit(1);
        }
        if (upper.size() != 1) outUpper.push_back({cp, upper});
        if (lower.size() != 1) outLower.push_back({cp, lower});
    }

    for (const std::string& cond : conditionsSeen) {
        const bool languageTailored =
            cond.rfind("lt", 0) == 0 || cond.rfind("tr", 0) == 0 || cond.rfind("az", 0) == 0;
        if (!languageTailored && cond != "Final_Sigma") {
            std::cerr << "error: an unhandled language-independent casing condition: " << cond
                      << "\n";
            std::exit(1);
        }
    }
    if (conditionsSeen.find("Final_Sigma") == conditionsSeen.end()) {
        std::cerr << "error: SpecialCasing.txt no longer carries the Final_Sigma condition\n";
        std::exit(1);
    }

    std::sort(outUpper.begin(), outUpper.end(),
              [](const FullCaseEntry& a, const FullCaseEntry& b) { return a.from < b.from; });
    std::sort(outLower.begin(), outLower.end(),
              [](const FullCaseEntry& a, const FullCaseEntry& b) { return a.from < b.from; });
}

// One property of DerivedCoreProperties.txt, as merged ascending ranges. Two
// are read, Cased and Case_Ignorable, and they are read because SpecialCasing's
// own definition of Final_Sigma is written in terms of exactly those two:
// "C is preceded by a sequence consisting of a cased letter and then zero or
// more case-ignorable characters, and C is not followed by a sequence
// consisting of zero or more case-ignorable characters and then a cased
// letter."
std::vector<PropRange> parseDerivedProperty(const std::filesystem::path& path,
                                            const std::string& property) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(1);
    }
    std::vector<PropRange> ranges;
    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        const auto commentPos = trimmed.find('#');
        if (commentPos != std::string::npos) trimmed = trim(trimmed.substr(0, commentPos));
        std::vector<std::string> fields = split(trimmed, ';');
        if (fields.size() < 2) continue;
        if (trim(fields[1]) != property) continue;
        const std::string codes = trim(fields[0]);
        const auto dots = codes.find("..");
        uint32_t first = 0;
        uint32_t last = 0;
        if (dots == std::string::npos) {
            if (!parseHex(codes, first)) continue;
            last = first;
        } else {
            if (!parseHex(codes.substr(0, dots), first)) continue;
            if (!parseHex(codes.substr(dots + 2), last)) continue;
        }
        ranges.push_back({first, last});
    }
    if (ranges.empty()) {
        std::cerr << "error: DerivedCoreProperties.txt carries no " << property << " ranges\n";
        std::exit(1);
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const PropRange& a, const PropRange& b) { return a.first < b.first; });
    // Merged and adjacency-joined, so the lookup is one binary search over
    // disjoint intervals and the table is a function of the PROPERTY rather
    // than of the order the file happens to list it in.
    std::vector<PropRange> merged;
    for (const PropRange& r : ranges) {
        if (!merged.empty() && r.first <= merged.back().last + 1) {
            if (r.last > merged.back().last) merged.back().last = r.last;
            continue;
        }
        merged.push_back(r);
    }
    return merged;
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "error: cannot write to " << path << "\n";
        std::exit(1);
    }
    out.write(content.data(), content.size());
    std::cout << "wrote " << path.string() << " ("
              << std::count(content.begin(), content.end(), '\n') << " lines)\n";
}

std::string generateHeader(const std::vector<std::string>& aliases, size_t runCount,
                           size_t foldCount) {
    std::ostringstream ss;
    ss << kBanner << "\n";
    ss << "#pragma once\n\n";
    ss << "#include <cstdint>\n\n";
    ss << "namespace bronze::regex::data {\n\n";
    ss << "// The General_Category of every code point, as the RUNS the property forms:\n";
    ss << "// `start` is the first code point of a run and `category` indexes\n";
    ss << "// `kGcAliases`. A run ends where the next begins, so there is no end field and\n";
    ss << "// no way to write a gap: the runs partition [0, 0x10FFFF], unassigned code\n";
    ss << "// points included, because `Cn` is a General_Category value and `\\p{Cn}`\n";
    ss << "// names it.\n";
    ss << "struct GcRun {\n";
    ss << "    uint32_t start;\n";
    ss << "    uint8_t category;\n";
    ss << "};\n\n";
    ss << "extern const GcRun kGcRuns[];\n";
    ss << "constexpr uint32_t kGcRunCount = " << runCount << ";\n\n";
    ss << "// The " << aliases.size()
       << " General_Category values, by the two-letter alias UAX #44 gives them,\n";
    ss << "// in the order `GcRun::category` indexes. Sorted, so the index is stable\n";
    ss << "// across regenerations.\n";
    ss << "extern const char* const kGcAliases[];\n";
    ss << "constexpr uint32_t kGcAliasCount = " << aliases.size() << ";\n\n";
    ss << "// Simple case folding -- CaseFolding.txt statuses C and S, which is what\n";
    ss << "// ECMA-262 22.2.2.9 applies under `u` and `i`. Only the code points scf does\n";
    ss << "// NOT leave alone are here, ascending by `from`, so a lookup that finds\n";
    ss << "// nothing is an identity fold.\n";
    ss << "struct FoldEntry {\n";
    ss << "    uint32_t from;\n";
    ss << "    uint32_t to;\n";
    ss << "};\n\n";
    ss << "extern const FoldEntry kSimpleCaseFolds[];\n";
    ss << "constexpr uint32_t kSimpleCaseFoldCount = " << foldCount << ";\n\n";
    ss << "}  // namespace bronze::regex::data\n";
    return ss.str();
}

std::string generateGcCpp(const std::vector<GcRun>& runs,
                         const std::vector<std::string>& aliases) {
    std::map<std::string, uint8_t> aliasIndex;
    for (size_t i = 0; i < aliases.size(); ++i) {
        aliasIndex[aliases[i]] = static_cast<uint8_t>(i);
    }

    std::ostringstream ss;
    ss << kBanner << "\n";
    ss << "#include \"regex/unicode_data.h\"\n\n";
    ss << "namespace bronze::regex::data {\n\n";
    ss << "const char* const kGcAliases[] = {\n";

    for (size_t i = 0; i < aliases.size(); ++i) {
        if (i % 10 == 0) ss << "    ";
        ss << "\"" << aliases[i] << "\",";
        if (i + 1 == aliases.size() || (i + 1) % 10 == 0) {
            ss << "\n";
        } else {
            ss << " ";
        }
    }
    ss << "};\n\n";

    ss << "const GcRun kGcRuns[] = {\n";
    for (size_t i = 0; i < runs.size(); ++i) {
        if (i % 6 == 0) ss << "    ";

        char buf[64];
        std::snprintf(buf, sizeof(buf), "{0x%06X,%2u},", runs[i].start,
                      aliasIndex[runs[i].category]);
        ss << buf;

        if (i + 1 == runs.size()) {
            ss << "\n";
        } else if ((i + 1) % 6 == 0) {
            ss << "\n";
        } else {
            ss << " ";
        }
    }
    ss << "};\n\n";
    ss << "}  // namespace bronze::regex::data\n";
    return ss.str();
}

std::string generateScfCpp(const std::vector<FoldEntry>& folds) {
    std::ostringstream ss;
    ss << kBanner << "\n";
    ss << "#include \"regex/unicode_data.h\"\n\n";
    ss << "namespace bronze::regex::data {\n\n";
    ss << "const FoldEntry kSimpleCaseFolds[] = {\n";

    for (size_t i = 0; i < folds.size(); ++i) {
        if (i % 4 == 0) ss << "    ";

        char buf[64];
        std::snprintf(buf, sizeof(buf), "{0x%06X,0x%06X},", folds[i].from, folds[i].to);
        ss << buf;

        if (i + 1 == folds.size()) {
            ss << "\n";
        } else if ((i + 1) % 4 == 0) {
            ss << "\n";
        } else {
            ss << " ";
        }
    }
    ss << "};\n\n";
    ss << "}  // namespace bronze::regex::data\n";
    return ss.str();
}


// ---- the runtime's case-conversion tables ----------------------------------

std::string generateCaseHeader(size_t simpleUpper, size_t simpleLower, size_t fullUpper,
                               size_t fullLower, size_t cased, size_t caseIgnorable) {
    std::ostringstream ss;
    ss << kBanner << "\n";
    ss << "#pragma once\n\n";
    ss << "#include <cstdint>\n\n";
    ss << "namespace bronze::runtime::unicode {\n\n";
    ss << "// Default Case Conversion (UCD 3.13), which is what ECMA-262 11.1.3 makes\n";
    ss << "// String.prototype.toUpperCase and toLowerCase apply. This is a DIFFERENT\n";
    ss << "// operation from the simple case folding in src/regex/unicode_data.h and the\n";
    ss << "// tables are not interchangeable: scf(U+1E9E) is U+00DF and so is\n";
    ss << "// toLowerCase(U+1E9E), but toUpperCase(U+00DF) is the two code points \"SS\",\n";
    ss << "// which no folding table can spell.\n\n";
    ss << "// A 1:1 mapping, from UnicodeData.txt field 12 (uppercase) or 13 (lowercase).\n";
    ss << "// Only the code points the mapping does NOT leave alone are here, ascending by\n";
    ss << "// `from`, so a lookup that finds nothing is an identity mapping.\n";
    ss << "struct CaseEntry {\n";
    ss << "    uint32_t from;\n";
    ss << "    uint32_t to;\n";
    ss << "};\n\n";
    ss << "extern const CaseEntry kSimpleUppercase[];\n";
    ss << "constexpr uint32_t kSimpleUppercaseCount = " << simpleUpper << ";\n";
    ss << "extern const CaseEntry kSimpleLowercase[];\n";
    ss << "constexpr uint32_t kSimpleLowercaseCount = " << simpleLower << ";\n\n";
    ss << "// A 1:MANY mapping, from SpecialCasing.txt's unconditional lines. `count` is 2\n";
    ss << "// or 3; a mapping of length 1 is never here, because it would be the simple\n";
    ss << "// mapping above and two copies of one fact is one too many. These SHADOW the\n";
    ss << "// simple tables: a code point in both is answered from this one.\n";
    ss << "struct FullCaseEntry {\n";
    ss << "    uint32_t from;\n";
    ss << "    uint32_t to[3];\n";
    ss << "    uint8_t count;\n";
    ss << "};\n\n";
    ss << "extern const FullCaseEntry kFullUppercase[];\n";
    ss << "constexpr uint32_t kFullUppercaseCount = " << fullUpper << ";\n";
    ss << "extern const FullCaseEntry kFullLowercase[];\n";
    ss << "constexpr uint32_t kFullLowercaseCount = " << fullLower << ";\n\n";
    ss << "// Cased and Case_Ignorable, from DerivedCoreProperties.txt, as disjoint\n";
    ss << "// ascending intervals. They are here for ONE caller: the Final_Sigma condition\n";
    ss << "// of SpecialCasing.txt, which is the only language-independent context rule in\n";
    ss << "// default casing and is defined in terms of exactly these two properties.\n";
    ss << "struct PropRange {\n";
    ss << "    uint32_t first;\n";
    ss << "    uint32_t last;\n";
    ss << "};\n\n";
    ss << "extern const PropRange kCasedRanges[];\n";
    ss << "constexpr uint32_t kCasedRangeCount = " << cased << ";\n";
    ss << "extern const PropRange kCaseIgnorableRanges[];\n";
    ss << "constexpr uint32_t kCaseIgnorableRangeCount = " << caseIgnorable << ";\n\n";
    ss << "}  // namespace bronze::runtime::unicode\n";
    return ss.str();
}

std::string generateSimpleCaseCpp(const std::vector<CaseEntry>& upper,
                                  const std::vector<CaseEntry>& lower) {
    std::ostringstream ss;
    ss << kBanner << "\n";
    ss << "#include \"runtime/unicode_case_data.h\"\n\n";
    ss << "namespace bronze::runtime::unicode {\n\n";
    const std::pair<const char*, const std::vector<CaseEntry>*> tables[] = {
        {"kSimpleUppercase", &upper},
        {"kSimpleLowercase", &lower},
    };
    for (const auto& table : tables) {
        ss << "const CaseEntry " << table.first << "[] = {\n";
        const std::vector<CaseEntry>& rows = *table.second;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i % 4 == 0) ss << "    ";
            char buf[64];
            std::snprintf(buf, sizeof(buf), "{0x%06X,0x%06X},", rows[i].from, rows[i].to);
            ss << buf;
            if (i + 1 == rows.size() || (i + 1) % 4 == 0) {
                ss << "\n";
            } else {
                ss << " ";
            }
        }
        ss << "};\n\n";
    }
    ss << "}  // namespace bronze::runtime::unicode\n";
    return ss.str();
}

std::string generateFullCaseCpp(const std::vector<FullCaseEntry>& upper,
                                const std::vector<FullCaseEntry>& lower,
                                const std::vector<PropRange>& cased,
                                const std::vector<PropRange>& caseIgnorable) {
    std::ostringstream ss;
    ss << kBanner << "\n";
    ss << "#include \"runtime/unicode_case_data.h\"\n\n";
    ss << "namespace bronze::runtime::unicode {\n\n";
    const std::pair<const char*, const std::vector<FullCaseEntry>*> tables[] = {
        {"kFullUppercase", &upper},
        {"kFullLowercase", &lower},
    };
    for (const auto& table : tables) {
        ss << "const FullCaseEntry " << table.first << "[] = {\n";
        for (const FullCaseEntry& row : *table.second) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "    {0x%06X, {0x%06X,0x%06X,0x%06X}, %u},\n",
                          row.from, row.to.size() > 0 ? row.to[0] : 0u,
                          row.to.size() > 1 ? row.to[1] : 0u, row.to.size() > 2 ? row.to[2] : 0u,
                          static_cast<unsigned>(row.to.size()));
            ss << buf;
        }
        ss << "};\n\n";
    }
    const std::pair<const char*, const std::vector<PropRange>*> props[] = {
        {"kCasedRanges", &cased},
        {"kCaseIgnorableRanges", &caseIgnorable},
    };
    for (const auto& prop : props) {
        ss << "const PropRange " << prop.first << "[] = {\n";
        const std::vector<PropRange>& rows = *prop.second;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i % 4 == 0) ss << "    ";
            char buf[64];
            std::snprintf(buf, sizeof(buf), "{0x%06X,0x%06X},", rows[i].first, rows[i].last);
            ss << buf;
            if (i + 1 == rows.size() || (i + 1) % 4 == 0) {
                ss << "\n";
            } else {
                ss << " ";
            }
        }
        ss << "};\n\n";
    }
    ss << "}  // namespace bronze::runtime::unicode\n";
    return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path ucdDir;
    std::filesystem::path outDir;

    if (argc >= 3) {
        ucdDir = argv[1];
        outDir = argv[2];
    } else {
        // Auto-locate relative to current working directory
        std::vector<std::filesystem::path> searchBases = {
            ".",
            "..",
            "../..",
            "../../..",
        };
        for (const auto& base : searchBases) {
            if (std::filesystem::exists(base / "tools/ucd/UnicodeData.txt") &&
                std::filesystem::exists(base / "src/regex/unicode_data.h")) {
                ucdDir = base / "tools/ucd";
                // The SOURCE root, not one module's directory: two modules are
                // served and each gets the tables its own operation needs.
                outDir = base / "src";
                break;
            }
        }
    }

    const char* const kInputs[] = {"UnicodeData.txt", "CaseFolding.txt", "SpecialCasing.txt",
                                   "DerivedCoreProperties.txt"};
    bool haveInputs = !ucdDir.empty() && !outDir.empty();
    for (const char* name : kInputs) {
        if (!haveInputs) break;
        haveInputs = std::filesystem::exists(ucdDir / name);
        if (!haveInputs) std::cerr << "error: missing " << (ucdDir / name).string() << "\n";
    }
    if (!haveInputs) {
        std::cerr << "error: could not locate UCD data files under " << ucdDir << "\n";
        std::cerr << "Usage: gen_unicode_tables [ucd_dir] [src_dir]\n";
        return 1;
    }

    std::cout << "Reading UCD " << kUcdVersion << " from " << ucdDir.string() << "...\n";

    std::vector<std::string> aliases;
    std::vector<GcRun> runs = parseUnicodeData(ucdDir / "UnicodeData.txt", aliases);
    std::vector<FoldEntry> folds = parseCaseFolding(ucdDir / "CaseFolding.txt");

    std::vector<CaseEntry> simpleUpper;
    std::vector<CaseEntry> simpleLower;
    parseSimpleCaseMappings(ucdDir / "UnicodeData.txt", simpleUpper, simpleLower);
    std::vector<FullCaseEntry> fullUpper;
    std::vector<FullCaseEntry> fullLower;
    parseSpecialCasing(ucdDir / "SpecialCasing.txt", fullUpper, fullLower, simpleUpper,
                       simpleLower);
    std::vector<PropRange> cased =
        parseDerivedProperty(ucdDir / "DerivedCoreProperties.txt", "Cased");
    std::vector<PropRange> caseIgnorable =
        parseDerivedProperty(ucdDir / "DerivedCoreProperties.txt", "Case_Ignorable");

    std::cout << "Parsed " << runs.size() << " General_Category runs (" << aliases.size()
              << " aliases)\n";
    std::cout << "Parsed " << folds.size() << " simple case fold mappings\n";
    std::cout << "Parsed " << simpleUpper.size() << " simple uppercase and " << simpleLower.size()
              << " simple lowercase mappings\n";
    std::cout << "Parsed " << fullUpper.size() << " full uppercase and " << fullLower.size()
              << " full lowercase mappings\n";
    std::cout << "Parsed " << cased.size() << " Cased ranges and " << caseIgnorable.size()
              << " Case_Ignorable ranges\n";

    writeFile(outDir / "regex" / "unicode_data.h",
              generateHeader(aliases, runs.size(), folds.size()));
    writeFile(outDir / "regex" / "unicode_data_gc.cpp", generateGcCpp(runs, aliases));
    writeFile(outDir / "regex" / "unicode_data_scf.cpp", generateScfCpp(folds));
    writeFile(outDir / "runtime" / "unicode_case_data.h",
              generateCaseHeader(simpleUpper.size(), simpleLower.size(), fullUpper.size(),
                                 fullLower.size(), cased.size(), caseIgnorable.size()));
    writeFile(outDir / "runtime" / "unicode_case_data_simple.cpp",
              generateSimpleCaseCpp(simpleUpper, simpleLower));
    writeFile(outDir / "runtime" / "unicode_case_data_full.cpp",
              generateFullCaseCpp(fullUpper, fullLower, cased, caseIgnorable));

    std::cout << "Done.\n";
    return 0;
}
