#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// What both halves of the generator share: the UCD version they assert, the
// banner every generated file opens with, and the reading of one UCD data line.
//
// Two translation units emit tables, one per module served, because the tables
// they emit answer different questions and their structural checks are written
// in different terms. What they do NOT differ about is the file format --
// every file under tools/ucd is `;`-separated fields with `#` opening a comment
// -- so that reading lives here and the knowledge of what each table MEANS
// lives beside the table.

namespace gen {

constexpr uint32_t kMaxCodePoint = 0x10FFFF;
constexpr const char* kUcdVersion = "16.0.0";

inline const char* const kBanner =
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

inline std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

inline std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> elems;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}

inline bool parseHex(const std::string& s, uint32_t& val) {
    std::string t = trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    unsigned long v = std::strtoul(t.c_str(), &end, 16);
    if (*end != '\0') return false;
    val = static_cast<uint32_t>(v);
    return true;
}

// One data line with its comment removed and its fields trimmed. False for a
// line that carries no data at all -- blank, or comment to its first character
// -- so a caller's loop is `if (!dataFields(line, fields)) continue;` and never
// a chain of substring tests it could get subtly different from its neighbour's.
inline bool dataFields(const std::string& line, std::vector<std::string>& out) {
    std::string text = trim(line);
    const auto comment = text.find('#');
    if (comment != std::string::npos) text = trim(text.substr(0, comment));
    if (text.empty()) return false;
    out = split(text, ';');
    for (std::string& field : out) field = trim(field);
    return true;
}

// `first..last`, or a single code point, which is how every RANGE in the UCD's
// property files is spelled.
inline bool parseCodeRange(const std::string& text, uint32_t& first, uint32_t& last) {
    const auto dots = text.find("..");
    if (dots == std::string::npos) {
        if (!parseHex(text, first)) return false;
        last = first;
        return true;
    }
    return parseHex(text.substr(0, dots), first) && parseHex(text.substr(dots + 2), last);
}

// A closed code point interval of a derived property.
struct PropRange {
    uint32_t first;
    uint32_t last;
};

// One property of DerivedCoreProperties.txt, as merged ascending ranges. Both
// halves read it: the runtime for Cased / Case_Ignorable (Final_Sigma), the
// regex module for ID_Start / ID_Continue (identifier code points).
inline std::vector<PropRange> parseDerivedProperty(const std::filesystem::path& path,
                                                   const std::string& property) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(1);
    }
    std::vector<PropRange> ranges;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> fields;
        if (!dataFields(line, fields)) continue;
        if (fields.size() < 2) continue;
        if (fields[1] != property) continue;
        uint32_t first = 0;
        uint32_t last = 0;
        if (!parseCodeRange(fields[0], first, last)) continue;
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

inline void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "error: cannot write to " << path << "\n";
        std::exit(1);
    }
    out.write(content.data(), content.size());
    std::cout << "wrote " << path.string() << " ("
              << std::count(content.begin(), content.end(), '\n') << " lines)\n";
}

// The two halves. Each reads the UCD files its own tables are built from and
// writes only into the module directory those tables belong to.
void generateRegexTables(const std::filesystem::path& ucdDir,
                         const std::filesystem::path& outDir);
void generateRuntimeTables(const std::filesystem::path& ucdDir,
                           const std::filesystem::path& outDir);
void generateNormTables(const std::filesystem::path& ucdDir, const std::filesystem::path& outDir);

}  // namespace gen
