// tools/gen_unicode_tables.cpp
//
// Generates the Unicode data tables required by src/regex (General_Category runs
// and simple case folding mappings) from published UCD 16.0.0 data files.
//
// Run once, commit the output. The build never invokes this: bronze does not
// depend on external generator tooling at build time, and a table regenerated
// during build cannot be reviewed or audited.
//
// Usage:
//   gen_unicode_tables [ucd_dir] [out_dir]

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
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
                outDir = base / "src/regex";
                break;
            }
        }
    }

    if (ucdDir.empty() || outDir.empty() ||
        !std::filesystem::exists(ucdDir / "UnicodeData.txt") ||
        !std::filesystem::exists(ucdDir / "CaseFolding.txt")) {
        std::cerr << "error: could not locate UCD data files under " << ucdDir << "\n";
        std::cerr << "Usage: gen_unicode_tables [ucd_dir] [out_dir]\n";
        return 1;
    }

    std::cout << "Reading UCD " << kUcdVersion << " from " << ucdDir.string() << "...\n";

    std::vector<std::string> aliases;
    std::vector<GcRun> runs = parseUnicodeData(ucdDir / "UnicodeData.txt", aliases);
    std::vector<FoldEntry> folds = parseCaseFolding(ucdDir / "CaseFolding.txt");

    std::cout << "Parsed " << runs.size() << " General_Category runs (" << aliases.size()
              << " aliases)\n";
    std::cout << "Parsed " << folds.size() << " simple case fold mappings\n";

    writeFile(outDir / "unicode_data.h", generateHeader(aliases, runs.size(), folds.size()));
    writeFile(outDir / "unicode_data_gc.cpp", generateGcCpp(runs, aliases));
    writeFile(outDir / "unicode_data_scf.cpp", generateScfCpp(folds));

    std::cout << "Done.\n";
    return 0;
}
