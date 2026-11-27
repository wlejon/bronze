// Generates the UTS #51 properties of strings (ECMA-262 Table 67):
//   - Basic_Emoji
//   - Emoji_Keycap_Sequence
//   - RGI_Emoji_Flag_Sequence
//   - RGI_Emoji_Tag_Sequence
//   - RGI_Emoji_Modifier_Sequence
//   - RGI_Emoji_ZWJ_Sequence
//   (RGI_Emoji is the union of the six above, assembled at runtime in unicode.cpp.)
//
// Reads emoji-sequences.txt and emoji-zwj-sequences.txt (Unicode 16.0) under tools/ucd/.
// Output: src/regex/unicode_data_strings.h, src/regex/unicode_data_strings.cpp,
// and src/regex/unicode_data_strings_N.cpp (each < 960 lines).

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gen_unicode_common.h"

namespace gen {

namespace {

constexpr const char* kBaseProperties[] = {
    "Basic_Emoji",
    "Emoji_Keycap_Sequence",
    "RGI_Emoji_Flag_Sequence",
    "RGI_Emoji_Modifier_Sequence",
    "RGI_Emoji_Tag_Sequence",
    "RGI_Emoji_ZWJ_Sequence",
};
constexpr size_t kBasePropertyCount = sizeof(kBaseProperties) / sizeof(kBaseProperties[0]);

constexpr size_t kLineBudget = 960;
constexpr size_t kRangesPerLine = 5;
constexpr size_t kIntsPerLine = 16;

struct StringPropData {
    std::vector<PropRange> ranges;
    std::vector<std::vector<uint32_t>> sequences;
};

void verifyEmojiVersion(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(1);
    }
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        if (line.find("# Version: 16.0") != std::string::npos) {
            found = true;
            break;
        }
        if (!line.empty() && line[0] != '#') break;
    }
    if (!found) {
        std::cerr << "error: " << path << " does not declare # Version: 16.0\n";
        std::exit(1);
    }
}

bool isKnownBaseProperty(const std::string& name) {
    for (const char* k : kBaseProperties) {
        if (name == k) return true;
    }
    return false;
}

std::string symSuffix(const std::string& property) {
    std::string out = "kStr";
    for (char c : property) {
        if (c != '_') out.push_back(c);
    }
    return out;
}

void mergeRanges(std::vector<PropRange>& ranges) {
    if (ranges.empty()) return;
    std::sort(ranges.begin(), ranges.end(),
              [](const PropRange& a, const PropRange& b) { return a.first < b.first; });
    std::vector<PropRange> merged;
    merged.reserve(ranges.size());
    for (const PropRange& r : ranges) {
        if (!merged.empty() && r.first <= merged.back().last + 1) {
            if (r.last > merged.back().last) merged.back().last = r.last;
        } else {
            merged.push_back(r);
        }
    }
    ranges = std::move(merged);
}

void parseEmojiFile(const std::filesystem::path& path,
                    std::map<std::string, StringPropData>& props) {
    verifyEmojiVersion(path);
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> fields;
        if (!dataFields(line, fields)) continue;
        if (fields.size() < 2) continue;
        const std::string& spec = fields[0];
        const std::string& prop = fields[1];
        if (!isKnownBaseProperty(prop)) {
            std::cerr << "error: unknown property `" << prop << "` in " << path << "\n";
            std::exit(1);
        }
        if (spec.find("..") != std::string::npos) {
            uint32_t first = 0;
            uint32_t last = 0;
            if (!parseCodeRange(spec, first, last)) {
                std::cerr << "error: bad range `" << spec << "` in " << path << "\n";
                std::exit(1);
            }
            props[prop].ranges.push_back({first, last});
            continue;
        }
        std::istringstream iss(spec);
        std::string tok;
        std::vector<uint32_t> seq;
        while (iss >> tok) {
            uint32_t cp = 0;
            if (!parseHex(tok, cp)) {
                std::cerr << "error: bad code point `" << tok << "` in " << path << "\n";
                std::exit(1);
            }
            seq.push_back(cp);
        }
        if (seq.empty()) continue;
        if (seq.size() == 1) {
            props[prop].ranges.push_back({seq[0], seq[0]});
        } else {
            props[prop].sequences.push_back(std::move(seq));
        }
    }
}

bool rangesContain(const std::vector<PropRange>& ranges, uint32_t cp) {
    for (const PropRange& r : ranges) {
        if (cp >= r.first && cp <= r.last) return true;
    }
    return false;
}

bool seqsContain(const std::vector<std::vector<uint32_t>>& seqs,
                 const std::vector<uint32_t>& target) {
    return std::binary_search(seqs.begin(), seqs.end(), target);
}

void checkStrings(const std::map<std::string, StringPropData>& props) {
    const auto& basic = props.at("Basic_Emoji");
    if (!rangesContain(basic.ranges, 0x1F600) || rangesContain(basic.ranges, 0x2764)) {
        std::cerr << "error: Basic_Emoji ranges failed sanity check\n";
        std::exit(1);
    }
    if (!seqsContain(basic.sequences, {0x2764, 0xFE0F})) {
        std::cerr << "error: Basic_Emoji missing {2764, FE0F}\n";
        std::exit(1);
    }
    if (!seqsContain(props.at("Emoji_Keycap_Sequence").sequences, {0x0031, 0xFE0F, 0x20E3})) {
        std::cerr << "error: Emoji_Keycap_Sequence missing keycap 1\n";
        std::exit(1);
    }
    if (!seqsContain(props.at("RGI_Emoji_Flag_Sequence").sequences, {0x1F1FA, 0x1F1F8})) {
        std::cerr << "error: RGI_Emoji_Flag_Sequence missing US flag\n";
        std::exit(1);
    }
    if (!seqsContain(props.at("RGI_Emoji_Tag_Sequence").sequences,
                     {0x1F3F4, 0xE0067, 0xE0062, 0xE0073, 0xE0063, 0xE0074, 0xE007F})) {
        std::cerr << "error: RGI_Emoji_Tag_Sequence missing Scotland flag\n";
        std::exit(1);
    }
    if (!seqsContain(props.at("RGI_Emoji_Modifier_Sequence").sequences, {0x1F44D, 0x1F3FE})) {
        std::cerr << "error: RGI_Emoji_Modifier_Sequence missing thumbs up dark-medium\n";
        std::exit(1);
    }
    if (!seqsContain(props.at("RGI_Emoji_ZWJ_Sequence").sequences,
                     {0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F466})) {
        std::cerr << "error: RGI_Emoji_ZWJ_Sequence missing family\n";
        std::exit(1);
    }
}

std::string fileHead() {
    std::ostringstream ss;
    ss << kBanner << "\n";
    ss << "#include \"regex/unicode_data_strings.h\"\n\n";
    ss << "namespace bronze::regex::data {\n\n";
    return ss.str();
}

constexpr const char* kFileTail = "}  // namespace bronze::regex::data\n";

std::vector<uint32_t> packSequences(const std::vector<std::vector<uint32_t>>& seqs) {
    std::vector<uint32_t> flat;
    for (const auto& s : seqs) {
        flat.push_back(static_cast<uint32_t>(s.size()));
        flat.insert(flat.end(), s.begin(), s.end());
    }
    return flat;
}

void emitU32Array(std::ostringstream& ss, const std::string& name,
                  const std::vector<uint32_t>& words) {
    ss << "const uint32_t " << name << "[] = {\n";
    for (size_t i = 0; i < words.size(); ++i) {
        if (i % kIntsPerLine == 0) ss << "   ";
        char buf[24];
        if (words[i] < 32) std::snprintf(buf, sizeof(buf), " %u,", words[i]);
        else std::snprintf(buf, sizeof(buf), " 0x%X,", words[i]);
        ss << buf;
        if (i + 1 == words.size() || (i + 1) % kIntsPerLine == 0) ss << "\n";
    }
    ss << "};\n\n";
}

size_t linesOfWords(size_t n) { return (n + kIntsPerLine - 1) / kIntsPerLine + 3; }

}  // namespace

void generateStringPropertyTables(const std::filesystem::path& ucdDir,
                                  const std::filesystem::path& outDir) {
    std::map<std::string, StringPropData> props;
    for (const char* name : kBaseProperties) props[name];

    parseEmojiFile(ucdDir / "emoji-sequences.txt", props);
    parseEmojiFile(ucdDir / "emoji-zwj-sequences.txt", props);

    for (auto& [name, data] : props) {
        mergeRanges(data.ranges);
        std::sort(data.sequences.begin(), data.sequences.end());
        data.sequences.erase(std::unique(data.sequences.begin(), data.sequences.end()),
                             data.sequences.end());
    }

    checkStrings(props);

    // Pack flat sequence words and shard under kLineBudget.
    const std::string head = fileHead();
    const size_t headLines = static_cast<size_t>(std::count(head.begin(), head.end(), '\n'));

    std::vector<std::vector<uint32_t>> packed(kBasePropertyCount);
    std::vector<std::string> files;
    std::ostringstream current;
    size_t currentLines = 0;
    size_t totalSeqs = 0;
    size_t totalRanges = 0;

    for (size_t i = 0; i < kBasePropertyCount; ++i) {
        const std::string name = kBaseProperties[i];
        const auto& data = props.at(name);
        packed[i] = packSequences(data.sequences);
        totalSeqs += data.sequences.size();
        totalRanges += data.ranges.size();

        const size_t lines = linesOfWords(packed[i].size());
        if (headLines + lines + 1 > kLineBudget) {
            std::cerr << "error: " << name << " overflows generated file budget (" << lines
                      << " lines)\n";
            std::exit(1);
        }
        if (currentLines > 0 && headLines + currentLines + lines + 1 > kLineBudget) {
            files.push_back(fileHead() + current.str() + kFileTail);
            current.str("");
            currentLines = 0;
        }
        emitU32Array(current, symSuffix(name) + "Seqs", packed[i]);
        currentLines += lines;
    }
    if (currentLines > 0) files.push_back(fileHead() + current.str() + kFileTail);

    // Header
    std::ostringstream h;
    h << kBanner << "\n";
    h << "#pragma once\n\n";
    h << "#include <cstdint>\n\n";
    h << "#include \"regex/unicode_data_binary.h\"\n\n";
    h << "namespace bronze::regex::data {\n\n";
    for (size_t i = 0; i < kBasePropertyCount; ++i) {
        const std::string name = kBaseProperties[i];
        const auto& data = props.at(name);
        if (!data.ranges.empty()) {
            h << "extern const CodeRange " << symSuffix(name) << "Ranges[];\n";
        }
        h << "extern const uint32_t " << symSuffix(name) << "Seqs[];\n";
    }
    h << "\nstruct StringPropertyTable {\n";
    h << "    const char* name;\n";
    h << "    const CodeRange* ranges;\n";
    h << "    uint32_t rangeCount;\n";
    h << "    const uint32_t* seqData;\n";
    h << "    uint32_t seqDataLen;\n";
    h << "};\n\n";
    h << "extern const StringPropertyTable kStringProperties[];\n";
    h << "constexpr uint32_t kStringPropertyCount = " << kBasePropertyCount << ";\n\n";
    h << kFileTail;

    // Index + ranges source
    std::ostringstream index;
    index << fileHead();
    for (size_t i = 0; i < kBasePropertyCount; ++i) {
        const std::string name = kBaseProperties[i];
        const auto& ranges = props.at(name).ranges;
        if (ranges.empty()) continue;
        index << "const CodeRange " << symSuffix(name) << "Ranges[] = {\n";
        for (size_t r = 0; r < ranges.size(); ++r) {
            if (r % kRangesPerLine == 0) index << "   ";
            char buf[48];
            std::snprintf(buf, sizeof(buf), " {0x%04X,0x%04X},", ranges[r].first, ranges[r].last);
            index << buf;
            if (r + 1 == ranges.size() || (r + 1) % kRangesPerLine == 0) index << "\n";
        }
        index << "};\n\n";
    }
    index << "const StringPropertyTable kStringProperties[] = {\n";
    for (size_t i = 0; i < kBasePropertyCount; ++i) {
        const std::string name = kBaseProperties[i];
        const auto& ranges = props.at(name).ranges;
        index << "    {\"" << name << "\", ";
        if (ranges.empty()) index << "nullptr, 0, ";
        else index << symSuffix(name) << "Ranges, " << ranges.size() << ", ";
        index << symSuffix(name) << "Seqs, " << packed[i].size() << "},\n";
    }
    index << "};\n\n";
    index << kFileTail;

    std::cout << "Parsed " << kBasePropertyCount << " string properties (" << totalRanges
              << " ranges, " << totalSeqs << " sequences)\n";

    const std::filesystem::path dir = outDir / "regex";
    writeFile(dir / "unicode_data_strings.h", h.str());
    writeFile(dir / "unicode_data_strings.cpp", index.str());
    for (size_t i = 0; i < files.size(); ++i) {
        writeFile(dir / ("unicode_data_strings_" + std::to_string(i + 1) + ".cpp"), files[i]);
    }
    for (size_t i = files.size() + 1;; ++i) {
        const std::filesystem::path stale =
            dir / ("unicode_data_strings_" + std::to_string(i) + ".cpp");
        if (!std::filesystem::exists(stale)) break;
        std::filesystem::remove(stale);
        std::cout << "removed " << stale.string() << "\n";
    }
}

}  // namespace gen
