// The regex module's half of the generated Unicode data: the General_Category
// runs `\p{...}` is answered from, the simple case FOLDING 22.2.2.9's
// Canonicalize applies under `u` and `i`, and the Script / Script_Extensions
// tables of UAX #24.
//
// All three are properties of a code point read straight out of the UCD, which
// is what puts them together and what separates them from the case CONVERSION
// tables in gen_unicode_runtime.cpp: those answer what a character turns INTO,
// and no folding or category table can spell that.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gen_unicode_common.h"

namespace gen {

namespace {

struct GcRun {
    uint32_t start;
    std::string category;
};

struct FoldEntry {
    uint32_t from;
    uint32_t to;
};

// A run of the Script property, in the shape the General_Category runs take:
// `script` indexes the sorted list of canonical script names.
struct ScriptRun {
    uint32_t start;
    uint16_t script;
};

// One line of ScriptExtensions.txt: a code point interval and the SET of
// scripts it belongs to, as indices into the same name list.
struct ScxRange {
    uint32_t first;
    uint32_t last;
    std::vector<uint16_t> set;
};

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
        std::vector<std::string> fields;
        if (!dataFields(line, fields)) continue;
        if (fields.size() < 3) continue;

        const std::string& status = fields[1];
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

// ---- Script and Script_Extensions (UAX #24) ---------------------------------

// PropertyValueAliases.txt's `sc` rows, which are the WHOLE of what a pattern
// may write: 22.2.1 UnicodeMatchPropertyValue matches a value against the
// names and aliases this file lists, exactly, so a script bronze accepts and a
// spelling it accepts both come from here rather than from a list written out
// by hand.
//
// The canonical name is the third field, the long one. `names` is sorted so
// the index a run carries is stable across regenerations; `aliases` holds every
// spelling, the four-letter code and the occasional third alias (`Qaac` for
// Coptic) included.
void parseScriptAliases(const std::filesystem::path& path, std::vector<std::string>& names,
                        std::map<std::string, uint16_t>& aliases) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(1);
    }
    std::vector<std::vector<std::string>> rows;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> fields;
        if (!dataFields(line, fields)) continue;
        if (fields.size() < 3 || fields[0] != "sc") continue;
        rows.push_back(fields);
    }
    if (rows.empty()) {
        std::cerr << "error: PropertyValueAliases.txt carries no `sc` rows\n";
        std::exit(1);
    }
    for (const std::vector<std::string>& row : rows) names.push_back(row[2]);
    std::sort(names.begin(), names.end());
    if (std::adjacent_find(names.begin(), names.end()) != names.end()) {
        std::cerr << "error: two `sc` rows share a canonical name\n";
        std::exit(1);
    }
    std::map<std::string, uint16_t> index;
    for (size_t i = 0; i < names.size(); ++i) index[names[i]] = static_cast<uint16_t>(i);
    for (const std::vector<std::string>& row : rows) {
        const uint16_t id = index[row[2]];
        for (size_t i = 1; i < row.size(); ++i) {
            if (row[i].empty()) continue;
            const auto seen = aliases.find(row[i]);
            if (seen != aliases.end() && seen->second != id) {
                std::cerr << "error: `" << row[i] << "` names two scripts\n";
                std::exit(1);
            }
            aliases[row[i]] = id;
        }
    }
}

// Scripts.txt, as runs partitioning the whole code space. Its `@missing` line
// gives every code point it does not list the value `Unknown`, which is a real
// Script value and not a hole -- so `\p{Script=Unknown}` names the unassigned
// code points and the runs need no end field, exactly as the General_Category
// runs do not.
std::vector<ScriptRun> parseScripts(const std::filesystem::path& path,
                                    const std::map<std::string, uint16_t>& aliases,
                                    uint16_t unknown) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(1);
    }
    std::vector<uint16_t> script(static_cast<size_t>(kMaxCodePoint) + 1, unknown);
    std::string line;
    size_t rows = 0;
    while (std::getline(in, line)) {
        std::vector<std::string> fields;
        if (!dataFields(line, fields)) continue;
        if (fields.size() < 2) continue;
        uint32_t first = 0;
        uint32_t last = 0;
        if (!parseCodeRange(fields[0], first, last) || last > kMaxCodePoint) {
            std::cerr << "error: unreadable code range in Scripts.txt: " << line << "\n";
            std::exit(1);
        }
        const auto it = aliases.find(fields[1]);
        if (it == aliases.end()) {
            std::cerr << "error: Scripts.txt names `" << fields[1]
                      << "`, which PropertyValueAliases.txt does not list as a script\n";
            std::exit(1);
        }
        for (uint32_t cp = first; cp <= last; ++cp) script[cp] = it->second;
        ++rows;
    }
    if (rows == 0) {
        std::cerr << "error: Scripts.txt carries no assignments\n";
        std::exit(1);
    }
    std::vector<ScriptRun> runs;
    for (uint32_t cp = 0; cp <= kMaxCodePoint; ++cp) {
        if (cp == 0 || script[cp] != script[cp - 1]) runs.push_back({cp, script[cp]});
    }
    return runs;
}

// ScriptExtensions.txt, which is a list of OVERRIDES and not a second copy of
// the property: its own header says every code point it does not list has its
// Script value as its Script_Extensions. Keeping it in that form is what makes
// the two tables provably consistent -- there is one Script table, and this
// names the code points that read differently.
std::vector<ScxRange> parseScriptExtensions(const std::filesystem::path& path,
                                            const std::map<std::string, uint16_t>& aliases) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(1);
    }
    std::vector<ScxRange> out;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> fields;
        if (!dataFields(line, fields)) continue;
        if (fields.size() < 2) continue;
        uint32_t first = 0;
        uint32_t last = 0;
        if (!parseCodeRange(fields[0], first, last) || last > kMaxCodePoint) {
            std::cerr << "error: unreadable code range in ScriptExtensions.txt: " << line << "\n";
            std::exit(1);
        }
        std::vector<uint16_t> set;
        std::istringstream words(fields[1]);
        std::string word;
        while (words >> word) {
            const auto it = aliases.find(word);
            if (it == aliases.end()) {
                std::cerr << "error: ScriptExtensions.txt names `" << word
                          << "`, which PropertyValueAliases.txt does not list as a script\n";
                std::exit(1);
            }
            set.push_back(it->second);
        }
        if (set.empty()) {
            std::cerr << "error: an empty Script_Extensions set: " << line << "\n";
            std::exit(1);
        }
        std::sort(set.begin(), set.end());
        set.erase(std::unique(set.begin(), set.end()), set.end());
        out.push_back({first, last, set});
    }
    if (out.empty()) {
        std::cerr << "error: ScriptExtensions.txt carries no assignments\n";
        std::exit(1);
    }
    std::sort(out.begin(), out.end(),
              [](const ScxRange& a, const ScxRange& b) { return a.first < b.first; });
    for (size_t i = 1; i < out.size(); ++i) {
        if (out[i].first <= out[i - 1].last) {
            std::cerr << "error: overlapping Script_Extensions ranges at U+" << std::hex
                      << out[i].first << "\n";
            std::exit(1);
        }
    }
    return out;
}

// The structural check for both script tables, written as the facts a wrong
// field index or a swapped alias column would get wrong. U+3099 is the one that
// matters most: it is the disagreement the two properties EXIST to express --
// Inherited by Script, Hiragana and Katakana by Script_Extensions -- so a
// generator that had quietly made scx a copy of sc would fail here and nowhere
// else.
void checkScripts(const std::vector<std::string>& names,
                  const std::map<std::string, uint16_t>& aliases,
                  const std::vector<ScriptRun>& runs, const std::vector<ScxRange>& scx) {
    auto idOf = [&](const std::string& alias) -> uint16_t {
        const auto it = aliases.find(alias);
        if (it == aliases.end()) {
            std::cerr << "error: no script is spelled `" << alias << "`\n";
            std::exit(1);
        }
        return it->second;
    };
    auto scriptAt = [&](uint32_t cp) -> const std::string& {
        const ScriptRun* found = &runs[0];
        for (const ScriptRun& run : runs) {
            if (run.start > cp) break;
            found = &run;
        }
        return names[found->script];
    };
    if (scriptAt(0x0041) != "Latin" || scriptAt(0x0391) != "Greek" ||
        scriptAt(0x4E00) != "Han" || scriptAt(0x0660) != "Arabic" ||
        scriptAt(0x0342) != "Inherited" || scriptAt(0x3099) != "Inherited" ||
        scriptAt(0x0378) != "Unknown") {
        std::cerr << "error: the Script runs disagree with Scripts.txt\n";
        std::exit(1);
    }
    if (names[idOf("Grek")] != "Greek" || names[idOf("Zzzz")] != "Unknown" ||
        names[idOf("Qaac")] != "Coptic") {
        std::cerr << "error: a script alias resolves to the wrong name\n";
        std::exit(1);
    }
    auto setAt = [&](uint32_t cp) -> std::vector<uint16_t> {
        for (const ScxRange& range : scx) {
            if (cp >= range.first && cp <= range.last) return range.set;
        }
        return {};
    };
    const std::vector<uint16_t> kana = {idOf("Hira"), idOf("Kana")};
    std::vector<uint16_t> expectedKana = kana;
    std::sort(expectedKana.begin(), expectedKana.end());
    if (setAt(0x3099) != expectedKana || setAt(0x0342) != std::vector<uint16_t>{idOf("Grek")}) {
        std::cerr << "error: the Script_Extensions overrides disagree with ScriptExtensions.txt\n";
        std::exit(1);
    }
    if (!setAt(0x0041).empty()) {
        std::cerr << "error: ScriptExtensions.txt has grown an override for U+0041, which would "
                     "make scx a second copy of sc rather than a list of exceptions\n";
        std::exit(1);
    }
}

// ---- emission ---------------------------------------------------------------

std::string generateHeader(const std::vector<std::string>& aliases, size_t runCount,
                           size_t foldCount, size_t scriptCount, size_t scriptRunCount,
                           size_t scriptAliasCount, size_t scxRangeCount, size_t scxScriptCount) {
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
    ss << "// The Script of every code point (UAX #24), in exactly the shape the\n";
    ss << "// General_Category runs take and for the same reason: Scripts.txt gives every\n";
    ss << "// code point it does not list the value `Unknown`, so the runs partition\n";
    ss << "// [0, 0x10FFFF] and `\\p{Script=Unknown}` names the rest.\n";
    ss << "struct ScriptRun {\n";
    ss << "    uint32_t start;\n";
    ss << "    uint16_t script;\n";
    ss << "};\n\n";
    ss << "extern const ScriptRun kScriptRuns[];\n";
    ss << "constexpr uint32_t kScriptRunCount = " << scriptRunCount << ";\n\n";
    ss << "// The " << scriptCount
       << " script values by their canonical long name, sorted, so the index\n";
    ss << "// `ScriptRun::script` carries is stable across regenerations.\n";
    ss << "extern const char* const kScriptNames[];\n";
    ss << "constexpr uint32_t kScriptCount = " << scriptCount << ";\n\n";
    ss << "// Every spelling PropertyValueAliases.txt gives a script -- the four-letter\n";
    ss << "// code, the long name, and the occasional third alias (`Qaac` for Coptic) --\n";
    ss << "// sorted by name. 22.2.1 matches a property value EXACTLY, so this list is the\n";
    ss << "// whole of what a pattern may write.\n";
    ss << "struct ScriptAlias {\n";
    ss << "    const char* name;\n";
    ss << "    uint16_t script;\n";
    ss << "};\n\n";
    ss << "extern const ScriptAlias kScriptAliases[];\n";
    ss << "constexpr uint32_t kScriptAliasCount = " << scriptAliasCount << ";\n\n";
    ss << "// Script_Extensions, as the OVERRIDES it is: ScriptExtensions.txt lists only\n";
    ss << "// the code points whose scx differs from their sc, and every other code point's\n";
    ss << "// scx is the one-element set holding its Script. `set` is an offset into\n";
    ss << "// `kScxScripts` and `count` is the length there. Sets are shared, so two\n";
    ss << "// ranges with the same scripts name one run of the table.\n";
    ss << "struct ScxRange {\n";
    ss << "    uint32_t first;\n";
    ss << "    uint32_t last;\n";
    ss << "    uint32_t set;\n";
    ss << "    uint32_t count;\n";
    ss << "};\n\n";
    ss << "extern const ScxRange kScxRanges[];\n";
    ss << "constexpr uint32_t kScxRangeCount = " << scxRangeCount << ";\n";
    ss << "extern const uint16_t kScxScripts[];\n";
    ss << "constexpr uint32_t kScxScriptCount = " << scxScriptCount << ";\n\n";
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

// The script tables, with the Script_Extensions sets INTERNED: 204 ranges share
// far fewer distinct sets, and a shared set is also the only form in which two
// ranges cannot come to disagree about what `Hira Kana` means.
std::string generateScriptCpp(const std::vector<std::string>& names,
                              const std::map<std::string, uint16_t>& aliases,
                              const std::vector<ScriptRun>& runs,
                              const std::vector<ScxRange>& scx, size_t& outScxScriptCount) {
    std::vector<uint16_t> flat;
    std::map<std::vector<uint16_t>, uint32_t> interned;
    std::vector<std::pair<uint32_t, uint32_t>> spans;  // offset, count per scx range
    for (const ScxRange& range : scx) {
        const auto seen = interned.find(range.set);
        if (seen != interned.end()) {
            spans.emplace_back(seen->second, static_cast<uint32_t>(range.set.size()));
            continue;
        }
        const auto offset = static_cast<uint32_t>(flat.size());
        interned.emplace(range.set, offset);
        flat.insert(flat.end(), range.set.begin(), range.set.end());
        spans.emplace_back(offset, static_cast<uint32_t>(range.set.size()));
    }
    outScxScriptCount = flat.size();

    std::ostringstream ss;
    ss << kBanner << "\n";
    ss << "#include \"regex/unicode_data.h\"\n\n";
    ss << "namespace bronze::regex::data {\n\n";

    ss << "const char* const kScriptNames[] = {\n";
    for (size_t i = 0; i < names.size(); ++i) {
        if (i % 4 == 0) ss << "    ";
        ss << "\"" << names[i] << "\",";
        if (i + 1 == names.size() || (i + 1) % 4 == 0) {
            ss << "\n";
        } else {
            ss << " ";
        }
    }
    ss << "};\n\n";

    ss << "const ScriptAlias kScriptAliases[] = {\n";
    {
        size_t i = 0;
        for (const auto& entry : aliases) {
            if (i % 3 == 0) ss << "    ";
            char buf[128];
            std::snprintf(buf, sizeof(buf), "{\"%s\",%u},", entry.first.c_str(),
                          static_cast<unsigned>(entry.second));
            ss << buf;
            ++i;
            if (i == aliases.size() || i % 3 == 0) {
                ss << "\n";
            } else {
                ss << " ";
            }
        }
    }
    ss << "};\n\n";

    ss << "const ScriptRun kScriptRuns[] = {\n";
    for (size_t i = 0; i < runs.size(); ++i) {
        if (i % 6 == 0) ss << "    ";
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{0x%06X,%3u},", runs[i].start, runs[i].script);
        ss << buf;
        if (i + 1 == runs.size() || (i + 1) % 6 == 0) {
            ss << "\n";
        } else {
            ss << " ";
        }
    }
    ss << "};\n\n";

    ss << "const uint16_t kScxScripts[] = {\n";
    for (size_t i = 0; i < flat.size(); ++i) {
        if (i % 12 == 0) ss << "    ";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%3u,", flat[i]);
        ss << buf;
        if (i + 1 == flat.size() || (i + 1) % 12 == 0) {
            ss << "\n";
        } else {
            ss << " ";
        }
    }
    ss << "};\n\n";

    ss << "const ScxRange kScxRanges[] = {\n";
    for (size_t i = 0; i < scx.size(); ++i) {
        if (i % 2 == 0) ss << "    ";
        char buf[96];
        std::snprintf(buf, sizeof(buf), "{0x%06X,0x%06X,%5u,%2u},", scx[i].first, scx[i].last,
                      spans[i].first, spans[i].second);
        ss << buf;
        if (i + 1 == scx.size() || (i + 1) % 2 == 0) {
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

void generateRegexTables(const std::filesystem::path& ucdDir,
                         const std::filesystem::path& outDir) {
    std::vector<std::string> aliases;
    std::vector<GcRun> runs = parseUnicodeData(ucdDir / "UnicodeData.txt", aliases);
    std::vector<FoldEntry> folds = parseCaseFolding(ucdDir / "CaseFolding.txt");

    std::vector<std::string> scriptNames;
    std::map<std::string, uint16_t> scriptAliases;
    parseScriptAliases(ucdDir / "PropertyValueAliases.txt", scriptNames, scriptAliases);
    const auto unknown = scriptAliases.find("Unknown");
    if (unknown == scriptAliases.end()) {
        std::cerr << "error: PropertyValueAliases.txt has no `Unknown` script, which is the "
                     "value Scripts.txt gives every code point it does not list\n";
        std::exit(1);
    }
    std::vector<ScriptRun> scriptRuns =
        parseScripts(ucdDir / "Scripts.txt", scriptAliases, unknown->second);
    std::vector<ScxRange> scx =
        parseScriptExtensions(ucdDir / "ScriptExtensions.txt", scriptAliases);
    checkScripts(scriptNames, scriptAliases, scriptRuns, scx);

    std::cout << "Parsed " << runs.size() << " General_Category runs (" << aliases.size()
              << " aliases)\n";
    std::cout << "Parsed " << folds.size() << " simple case fold mappings\n";
    std::cout << "Parsed " << scriptRuns.size() << " Script runs (" << scriptNames.size()
              << " scripts, " << scriptAliases.size() << " spellings) and " << scx.size()
              << " Script_Extensions overrides\n";

    size_t scxScriptCount = 0;
    const std::string scriptCpp =
        generateScriptCpp(scriptNames, scriptAliases, scriptRuns, scx, scxScriptCount);

    writeFile(outDir / "regex" / "unicode_data.h",
              generateHeader(aliases, runs.size(), folds.size(), scriptNames.size(),
                             scriptRuns.size(), scriptAliases.size(), scx.size(),
                             scxScriptCount));
    writeFile(outDir / "regex" / "unicode_data_gc.cpp", generateGcCpp(runs, aliases));
    writeFile(outDir / "regex" / "unicode_data_scf.cpp", generateScfCpp(folds));
    writeFile(outDir / "regex" / "unicode_data_script.cpp", scriptCpp);
}

}  // namespace gen
