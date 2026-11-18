// The binary Unicode properties `\p{...}` reads in its lone form: ECMA-262
// Table 69, less the three that need no data (`Any`, `ASCII`, `Assigned`,
// answered in src/regex/unicode.cpp from what is already there).
//
// The list of properties and their aliases is ECMA-262's and is written out
// here, as Table 67 is in unicode.cpp: it is a closed list in the
// specification, changed only when an edition adds a row, and 22.2.1
// UnicodeMatchProperty matches against exactly these spellings. Each property's
// code points come from the UCD file that defines it, read with the same
// parser as ID_Start and Cased, and are emitted as merged closed ranges.
//
// Output: src/regex/unicode_data_binary.h (declarations), unicode_data_binary.cpp
// (the property table and the sorted name table) and unicode_data_binary_N.cpp
// (the range arrays, packed into files of fewer than 1000 lines).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gen_unicode_common.h"

namespace gen {

namespace {

// Table 69, in its own order (sorted by canonical name). `alias` is empty for
// the properties that have none.
struct BinarySpec {
    const char* name;
    const char* alias;
    const char* file;
};

constexpr const char* kPropList = "PropList.txt";
constexpr const char* kCore = "DerivedCoreProperties.txt";
constexpr const char* kEmoji = "emoji-data.txt";
constexpr const char* kBinary = "DerivedBinaryProperties.txt";
constexpr const char* kNorm = "DerivedNormalizationProps.txt";

constexpr BinarySpec kTable69[] = {
    {"ASCII_Hex_Digit", "AHex", kPropList},
    {"Alphabetic", "Alpha", kCore},
    {"Bidi_Control", "Bidi_C", kPropList},
    {"Bidi_Mirrored", "Bidi_M", kBinary},
    {"Case_Ignorable", "CI", kCore},
    {"Cased", "", kCore},
    {"Changes_When_Casefolded", "CWCF", kCore},
    {"Changes_When_Casemapped", "CWCM", kCore},
    {"Changes_When_Lowercased", "CWL", kCore},
    {"Changes_When_NFKC_Casefolded", "CWKCF", kNorm},
    {"Changes_When_Titlecased", "CWT", kCore},
    {"Changes_When_Uppercased", "CWU", kCore},
    {"Dash", "", kPropList},
    {"Default_Ignorable_Code_Point", "DI", kCore},
    {"Deprecated", "Dep", kPropList},
    {"Diacritic", "Dia", kPropList},
    {"Emoji", "", kEmoji},
    {"Emoji_Component", "EComp", kEmoji},
    {"Emoji_Modifier", "EMod", kEmoji},
    {"Emoji_Modifier_Base", "EBase", kEmoji},
    {"Emoji_Presentation", "EPres", kEmoji},
    {"Extended_Pictographic", "ExtPict", kEmoji},
    {"Extender", "Ext", kPropList},
    {"Grapheme_Base", "Gr_Base", kCore},
    {"Grapheme_Extend", "Gr_Ext", kCore},
    {"Hex_Digit", "Hex", kPropList},
    {"IDS_Binary_Operator", "IDSB", kPropList},
    {"IDS_Trinary_Operator", "IDST", kPropList},
    {"ID_Continue", "IDC", kCore},
    {"ID_Start", "IDS", kCore},
    {"Ideographic", "Ideo", kPropList},
    {"Join_Control", "Join_C", kPropList},
    {"Logical_Order_Exception", "LOE", kPropList},
    {"Lowercase", "Lower", kCore},
    {"Math", "", kCore},
    {"Noncharacter_Code_Point", "NChar", kPropList},
    {"Pattern_Syntax", "Pat_Syn", kPropList},
    {"Pattern_White_Space", "Pat_WS", kPropList},
    {"Quotation_Mark", "QMark", kPropList},
    {"Radical", "", kPropList},
    {"Regional_Indicator", "RI", kPropList},
    {"Sentence_Terminal", "STerm", kPropList},
    {"Soft_Dotted", "SD", kPropList},
    {"Terminal_Punctuation", "Term", kPropList},
    {"Unified_Ideograph", "UIdeo", kPropList},
    {"Uppercase", "Upper", kCore},
    {"Variation_Selector", "VS", kPropList},
    {"White_Space", "space", kPropList},
    {"XID_Continue", "XIDC", kCore},
    {"XID_Start", "XIDS", kCore},
};

constexpr size_t kPropertyCount = sizeof(kTable69) / sizeof(kTable69[0]);

// Under 1000 lines per generated file, with room for the banner.
constexpr size_t kLineBudget = 960;
constexpr size_t kRangesPerLine = 5;

// `Changes_When_NFKC_Casefolded` -> `kBinChangesWhenNFKCCasefolded`.
std::string arrayName(const std::string& property) {
    std::string out = "kBin";
    for (char c : property) {
        if (c != '_') out.push_back(c);
    }
    return out;
}

bool contains(const std::vector<PropRange>& ranges, uint32_t cp) {
    for (const PropRange& r : ranges) {
        if (cp >= r.first && cp <= r.last) return true;
    }
    return false;
}

// Facts a wrong file, a wrong field or a property read under another's name
// would get wrong.
void checkBinary(const std::map<std::string, std::vector<PropRange>>& props) {
    struct Fact {
        const char* property;
        uint32_t cp;
        bool member;
    };
    static constexpr Fact kFacts[] = {
        {"Ideographic", 0x4E00, true},          {"Ideographic", 0x0041, false},
        {"Unified_Ideograph", 0x3006, false},   {"Ideographic", 0x3006, true},
        {"White_Space", 0x0020, true},          {"White_Space", 0x200B, false},
        {"Alphabetic", 0x0345, true},           {"Alphabetic", 0x0030, false},
        {"Emoji", 0x1F600, true},               {"Emoji", 0x0023, true},
        {"Emoji_Presentation", 0x0023, false},  {"Extended_Pictographic", 0x00A9, true},
        {"Emoji_Modifier", 0x1F3FB, true},      {"Bidi_Mirrored", 0x0028, true},
        {"Bidi_Mirrored", 0x0041, false},       {"Changes_When_NFKC_Casefolded", 0x0041, true},
        {"Changes_When_NFKC_Casefolded", 0x0061, false},
        {"Noncharacter_Code_Point", 0xFFFF, true},
        {"Noncharacter_Code_Point", 0x10FFFF, true},
        {"Regional_Indicator", 0x1F1E6, true},  {"Lowercase", 0x00AA, true},
        {"Uppercase", 0x2160, true},            {"XID_Start", 0x037A, false},
        {"ID_Start", 0x037A, true},
    };
    for (const Fact& fact : kFacts) {
        const auto it = props.find(fact.property);
        if (it == props.end() || contains(it->second, fact.cp) != fact.member) {
            std::cerr << "error: " << fact.property << (fact.member ? " lacks" : " holds")
                      << " U+" << std::hex << fact.cp << ", which the UCD says it "
                      << (fact.member ? "holds" : "lacks") << "\n";
            std::exit(1);
        }
    }
    // ASCII_Hex_Digit is exactly [0-9A-Fa-f].
    const std::vector<PropRange>& ahex = props.at("ASCII_Hex_Digit");
    if (ahex.size() != 3 || ahex[0].first != '0' || ahex[0].last != '9' ||
        ahex[1].first != 'A' || ahex[1].last != 'F' || ahex[2].first != 'a' ||
        ahex[2].last != 'f') {
        std::cerr << "error: ASCII_Hex_Digit is not [0-9A-Fa-f]\n";
        std::exit(1);
    }
}

void emitRanges(std::ostringstream& ss, const std::string& property,
                const std::vector<PropRange>& ranges) {
    ss << "const CodeRange " << arrayName(property) << "[] = {\n";
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (i % kRangesPerLine == 0) ss << "   ";
        char buf[48];
        std::snprintf(buf, sizeof(buf), " {0x%04X,0x%04X},", ranges[i].first, ranges[i].last);
        ss << buf;
        if (i + 1 == ranges.size() || (i + 1) % kRangesPerLine == 0) ss << "\n";
    }
    ss << "};\n\n";
}

size_t linesOf(const std::vector<PropRange>& ranges) {
    return (ranges.size() + kRangesPerLine - 1) / kRangesPerLine + 3;
}

std::string fileHead() {
    std::ostringstream ss;
    ss << kBanner << "\n";
    ss << "#include \"regex/unicode_data_binary.h\"\n\n";
    ss << "namespace bronze::regex::data {\n\n";
    return ss.str();
}

constexpr const char* kFileTail = "}  // namespace bronze::regex::data\n";

}  // namespace

void generateBinaryTables(const std::filesystem::path& ucdDir,
                          const std::filesystem::path& outDir) {
    std::map<std::string, std::vector<PropRange>> props;
    std::vector<std::vector<PropRange>> ordered;
    std::map<std::string, uint8_t> names;  // every spelling -> index into kTable69
    for (size_t i = 0; i < kPropertyCount; ++i) {
        const BinarySpec& spec = kTable69[i];
        if (i > 0 && std::string(kTable69[i - 1].name) >= spec.name) {
            std::cerr << "error: Table 69 is not sorted at " << spec.name << "\n";
            std::exit(1);
        }
        std::vector<PropRange> ranges = parseDerivedProperty(ucdDir / spec.file, spec.name);
        props[spec.name] = ranges;
        ordered.push_back(std::move(ranges));
        for (const char* spelling : {spec.name, spec.alias}) {
            if (*spelling == '\0') continue;
            if (!names.emplace(spelling, static_cast<uint8_t>(i)).second) {
                std::cerr << "error: `" << spelling << "` names two binary properties\n";
                std::exit(1);
            }
        }
    }
    checkBinary(props);

    // The range arrays, packed in table order into files under the budget.
    const std::string head = fileHead();
    const size_t headLines = static_cast<size_t>(std::count(head.begin(), head.end(), '\n'));
    std::vector<std::string> files;
    std::ostringstream current;
    size_t currentLines = 0;
    size_t total = 0;
    for (size_t i = 0; i < kPropertyCount; ++i) {
        const size_t lines = linesOf(ordered[i]);
        if (headLines + lines + 1 > kLineBudget) {
            std::cerr << "error: " << kTable69[i].name << " alone overflows a generated file\n";
            std::exit(1);
        }
        if (currentLines > 0 && headLines + currentLines + lines + 1 > kLineBudget) {
            files.push_back(fileHead() + current.str() + kFileTail);
            current.str("");
            currentLines = 0;
        }
        emitRanges(current, kTable69[i].name, ordered[i]);
        currentLines += lines;
        total += ordered[i].size();
    }
    files.push_back(fileHead() + current.str() + kFileTail);

    std::ostringstream h;
    h << kBanner << "\n";
    h << "#pragma once\n\n";
    h << "#include <cstdint>\n\n";
    h << "namespace bronze::regex::data {\n\n";
    h << "// A closed code point interval.\n";
    h << "struct CodeRange {\n";
    h << "    uint32_t first;\n";
    h << "    uint32_t last;\n";
    h << "};\n\n";
    h << "// The binary properties of ECMA-262 Table 69 that carry data (all but `Any`,\n";
    h << "// `ASCII` and `Assigned`), each as merged ascending ranges, " << total
      << " in all.\n";
    for (size_t i = 0; i < kPropertyCount; ++i) {
        h << "extern const CodeRange " << arrayName(kTable69[i].name) << "[];\n";
    }
    h << "\n";
    h << "struct BinaryProperty {\n";
    h << "    const char* name;  // canonical\n";
    h << "    const CodeRange* ranges;\n";
    h << "    uint32_t count;\n";
    h << "};\n\n";
    h << "extern const BinaryProperty kBinaryProperties[];\n";
    h << "constexpr uint32_t kBinaryPropertyCount = " << kPropertyCount << ";\n\n";
    h << "// Every spelling Table 69 gives (canonical name and alias), sorted, with the\n";
    h << "// index into kBinaryProperties it names. 22.2.1 matches exactly.\n";
    h << "struct BinaryPropertyName {\n";
    h << "    const char* name;\n";
    h << "    uint8_t property;\n";
    h << "};\n\n";
    h << "extern const BinaryPropertyName kBinaryPropertyNames[];\n";
    h << "constexpr uint32_t kBinaryPropertyNameCount = " << names.size() << ";\n\n";
    h << kFileTail;

    std::ostringstream index;
    index << fileHead();
    index << "const BinaryProperty kBinaryProperties[] = {\n";
    for (size_t i = 0; i < kPropertyCount; ++i) {
        index << "    {\"" << kTable69[i].name << "\", " << arrayName(kTable69[i].name) << ", "
              << ordered[i].size() << "},\n";
    }
    index << "};\n\n";
    index << "const BinaryPropertyName kBinaryPropertyNames[] = {\n";
    {
        size_t i = 0;
        for (const auto& entry : names) {
            if (i % 3 == 0) index << "   ";
            index << " {\"" << entry.first << "\", " << static_cast<unsigned>(entry.second)
                  << "},";
            ++i;
            if (i == names.size() || i % 3 == 0) index << "\n";
        }
    }
    index << "};\n\n";
    index << kFileTail;

    std::cout << "Parsed " << kPropertyCount << " binary properties (" << names.size()
              << " spellings, " << total << " ranges)\n";

    const std::filesystem::path dir = outDir / "regex";
    writeFile(dir / "unicode_data_binary.h", h.str());
    writeFile(dir / "unicode_data_binary.cpp", index.str());
    for (size_t i = 0; i < files.size(); ++i) {
        writeFile(dir / ("unicode_data_binary_" + std::to_string(i + 1) + ".cpp"), files[i]);
    }
    // A stale file from a run that needed more would still be compiled.
    for (size_t i = files.size() + 1;; ++i) {
        const std::filesystem::path stale =
            dir / ("unicode_data_binary_" + std::to_string(i) + ".cpp");
        if (!std::filesystem::exists(stale)) break;
        std::filesystem::remove(stale);
        std::cout << "removed " << stale.string() << "\n";
    }
}

}  // namespace gen
