#include "regex/unicode.h"

#include <algorithm>
#include <map>

#include "regex/unicode_data.h"

namespace bronze::regex {

namespace {

static_assert(data::kGcAliasCount <= 32, "a General_Category set is carried in a uint32_t mask");

constexpr uint32_t bit(uint32_t index) { return 1u << index; }

uint32_t aliasIndex(std::string_view alias) {
    for (uint32_t i = 0; i < data::kGcAliasCount; ++i) {
        if (alias == data::kGcAliases[i]) return i;
    }
    return data::kGcAliasCount;
}

// What a pattern may write, and which General_Category values it stands for.
//
// The unions are spelled as their MEMBERS rather than as ranges of their own,
// which is the only form in which `\p{L}` and `\p{Lu}` cannot drift: there is
// one table of ranges, the generated one, and every name here is a mask over
// it. It is also what makes `\p{L}` provably the union of its five members
// rather than a second opinion about which characters are letters.
//
// Both spellings of every value are here — the two-letter alias UAX #44 gives
// it and the long name — because ECMA-262 Table 68 lists both and a pattern
// written with either is the same pattern. Matching is case SENSITIVE, as
// 22.2.1 requires: `\p{lu}` is a syntax error, not a lowercase `Lu`.
struct GcName {
    const char* text;
    const char* members;  // space-separated two-letter aliases
};

constexpr GcName kGcNames[] = {
    // The single-letter unions, and their long names.
    {"C", "Cc Cf Cn Co Cs"},
    {"Other", "Cc Cf Cn Co Cs"},
    {"L", "Ll Lm Lo Lt Lu"},
    {"Letter", "Ll Lm Lo Lt Lu"},
    // LC is the one two-letter name that is a union rather than a value: the
    // letters that HAVE a case, which is not the same as Lu plus Ll because a
    // titlecase letter is one too.
    {"LC", "Ll Lt Lu"},
    {"Cased_Letter", "Ll Lt Lu"},
    {"M", "Mc Me Mn"},
    {"Mark", "Mc Me Mn"},
    {"Combining_Mark", "Mc Me Mn"},
    {"N", "Nd Nl No"},
    {"Number", "Nd Nl No"},
    {"P", "Pc Pd Pe Pf Pi Po Ps"},
    {"Punctuation", "Pc Pd Pe Pf Pi Po Ps"},
    {"punct", "Pc Pd Pe Pf Pi Po Ps"},
    {"S", "Sc Sk Sm So"},
    {"Symbol", "Sc Sk Sm So"},
    {"Z", "Zl Zp Zs"},
    {"Separator", "Zl Zp Zs"},

    // The thirty values themselves.
    {"Cc", "Cc"},
    {"Control", "Cc"},
    {"cntrl", "Cc"},
    {"Cf", "Cf"},
    {"Format", "Cf"},
    {"Cn", "Cn"},
    {"Unassigned", "Cn"},
    {"Co", "Co"},
    {"Private_Use", "Co"},
    {"Cs", "Cs"},
    {"Surrogate", "Cs"},
    {"Ll", "Ll"},
    {"Lowercase_Letter", "Ll"},
    {"Lm", "Lm"},
    {"Modifier_Letter", "Lm"},
    {"Lo", "Lo"},
    {"Other_Letter", "Lo"},
    {"Lt", "Lt"},
    {"Titlecase_Letter", "Lt"},
    {"Lu", "Lu"},
    {"Uppercase_Letter", "Lu"},
    {"Mc", "Mc"},
    {"Spacing_Mark", "Mc"},
    {"Me", "Me"},
    {"Enclosing_Mark", "Me"},
    {"Mn", "Mn"},
    {"Nonspacing_Mark", "Mn"},
    {"Nd", "Nd"},
    {"Decimal_Number", "Nd"},
    {"digit", "Nd"},
    {"Nl", "Nl"},
    {"Letter_Number", "Nl"},
    {"No", "No"},
    {"Other_Number", "No"},
    {"Pc", "Pc"},
    {"Connector_Punctuation", "Pc"},
    {"Pd", "Pd"},
    {"Dash_Punctuation", "Pd"},
    {"Pe", "Pe"},
    {"Close_Punctuation", "Pe"},
    {"Pf", "Pf"},
    {"Final_Punctuation", "Pf"},
    {"Pi", "Pi"},
    {"Initial_Punctuation", "Pi"},
    {"Po", "Po"},
    {"Other_Punctuation", "Po"},
    {"Ps", "Ps"},
    {"Open_Punctuation", "Ps"},
    {"Sc", "Sc"},
    {"Currency_Symbol", "Sc"},
    {"Sk", "Sk"},
    {"Modifier_Symbol", "Sk"},
    {"Sm", "Sm"},
    {"Math_Symbol", "Sm"},
    {"So", "So"},
    {"Other_Symbol", "So"},
    {"Zl", "Zl"},
    {"Line_Separator", "Zl"},
    {"Zp", "Zp"},
    {"Paragraph_Separator", "Zp"},
    {"Zs", "Zs"},
    {"Space_Separator", "Zs"},
};

// The mask a name stands for, or zero when it is not one. Zero is unambiguous
// because no name here covers no category.
uint32_t maskOfName(std::string_view name) {
    for (const GcName& entry : kGcNames) {
        if (name != entry.text) continue;
        uint32_t mask = 0;
        std::string_view members(entry.members);
        while (!members.empty()) {
            const size_t space = members.find(' ');
            const std::string_view alias = members.substr(0, space);
            const uint32_t index = aliasIndex(alias);
            // A member that is not one of the generated aliases would make
            // `\p{L}` quietly smaller than the union it claims to be, so it is
            // a build-time impossibility rather than a runtime hole: the only
            // way to reach it is to write a typo in the table above, and the
            // test walking every name against the code space catches it.
            if (index == data::kGcAliasCount) return 0;
            mask |= bit(index);
            if (space == std::string_view::npos) break;
            members.remove_prefix(space + 1);
        }
        return mask;
    }
    return 0;
}

RangeList rangesForMask(uint32_t mask) {
    RangeList list;
    for (uint32_t i = 0; i < data::kGcRunCount; ++i) {
        if ((mask & bit(data::kGcRuns[i].category)) == 0) continue;
        // A run ends where the next begins; the last one ends at the ceiling.
        const uint32_t hi = i + 1 < data::kGcRunCount ? data::kGcRuns[i + 1].start - 1
                                                      : kMaxCodePoint;
        addRange(list, data::kGcRuns[i].start, hi);
    }
    // Adjacent runs of categories that are both in the mask merge here, which
    // is what makes `\p{L}` a handful of intervals rather than 1926 of them.
    normalizeRanges(list);
    return list;
}

// The three binary properties that follow from data bronze already has, and
// therefore need no table of their own. Every other UAX #44 binary property is
// an independent data file, which is exactly what this project does not carry.
bool binaryPropertySet(std::string_view value, RangeList& out) {
    if (value == "Any") {
        out.clear();
        addRange(out, 0, kMaxCodePoint);
        return true;
    }
    if (value == "ASCII") {
        out.clear();
        addRange(out, 0, 0x7F);
        return true;
    }
    if (value == "Assigned") {
        // Assigned is "has a General_Category other than Cn", which the
        // generated runs answer directly — the one binary property that is a
        // General_Category question in disguise.
        uint32_t mask = 0;
        for (uint32_t i = 0; i < data::kGcAliasCount; ++i) mask |= bit(i);
        out = rangesForMask(mask & ~bit(aliasIndex("Cn")));
        return true;
    }
    return false;
}

std::string quoted(std::string_view name, std::string_view value) {
    std::string out = "`\\p{";
    if (!name.empty()) {
        out.append(name);
        out.push_back('=');
    }
    out.append(value);
    out += "}`";
    return out;
}

// What bronze does carry, said once. Every refusal below ends with it, because
// a message that only says "no" leaves the reader guessing which of the two
// halves of UAX #44 is missing.
const char* kSupported =
    "bronze carries General_Category only: its 30 values by alias or long name "
    "(`\\p{Lu}`, `\\p{Uppercase_Letter}`, `\\p{General_Category=Lu}`), the unions "
    "`C L LC M N P S Z`, and the binary properties `ASCII`, `Any` and `Assigned`, "
    "which follow from the same table";

const std::vector<std::pair<uint32_t, uint32_t>>& foldTable() {
    static const std::vector<std::pair<uint32_t, uint32_t>> table = [] {
        std::vector<std::pair<uint32_t, uint32_t>> out;
        out.reserve(data::kSimpleCaseFoldCount);
        for (uint32_t i = 0; i < data::kSimpleCaseFoldCount; ++i) {
            out.emplace_back(data::kSimpleCaseFolds[i].from, data::kSimpleCaseFolds[i].to);
        }
        return out;
    }();
    return table;
}

const std::map<uint32_t, std::vector<uint32_t>>& reverseFoldTable() {
    static const std::map<uint32_t, std::vector<uint32_t>> table = [] {
        std::map<uint32_t, std::vector<uint32_t>> out;
        // Built from the entries and not from a walk over the code space: the
        // table already names every code point the fold moves, so the reverse
        // of it is complete by construction.
        for (const auto& entry : foldTable()) out[entry.second].push_back(entry.first);
        return out;
    }();
    return table;
}

}  // namespace

bool unicodePropertySet(std::string_view name, std::string_view value, RangeList& out,
                        std::string& error) {
    // Script first, and by name. It is the property a reader is most likely to
    // reach for after General_Category works, and letting it fall through to
    // the general refusal — or worse, to the lone-value lookup, where
    // `\p{Script=Greek}` would find no `Greek` category and read as a
    // misspelling — would hide which of the two tables is missing.
    if (name == "Script" || name == "sc" || name == "Script_Extensions" || name == "scx") {
        error = "unsupported: " + quoted(name, value) +
                " — the Unicode Script and Script_Extensions properties are not implemented. "
                "They are a separate UAX #24 data file, and the generator behind bronze's "
                "tables reads Python's `unicodedata`, which carries no Script property at "
                "all, so there is no honest source for one here. " +
                kSupported;
        return false;
    }
    if (!name.empty() && name != "General_Category" && name != "gc") {
        error = "unsupported: " + quoted(name, value) + " — `" + std::string(name) +
                "` is not a property bronze implements as a name=value pair. " + kSupported;
        return false;
    }

    if (name.empty() && binaryPropertySet(value, out)) return true;

    const uint32_t mask = maskOfName(value);
    if (mask == 0) {
        // One message for a misspelling and for a real property with no table,
        // because bronze genuinely cannot tell them apart — it has no list of
        // every UAX #44 property, and inventing one to sort the two cases would
        // be a guess. What it can do is name the offender and say what it does
        // have, which is what an unknown `\p{...}` most needs.
        error = "unsupported: " + quoted(name, value) + " names no property value bronze "
                "carries. It is either a misspelling or a UAX #44 property — a binary "
                "property such as `Alphabetic` or `White_Space`, or a Script — whose data "
                "bronze does not have. " + kSupported;
        return false;
    }
    out = rangesForMask(mask);
    return true;
}

std::string_view generalCategoryOf(uint32_t code) {
    const data::GcRun* begin = data::kGcRuns;
    const data::GcRun* end = begin + data::kGcRunCount;
    // The run CONTAINING `code`, which is the last one starting at or below it.
    // `kGcRuns[0].start` is 0, so there is always one.
    const data::GcRun* it = std::upper_bound(
        begin, end, code, [](uint32_t value, const data::GcRun& run) { return value < run.start; });
    return data::kGcAliases[(it - 1)->category];
}

uint32_t simpleCaseFold(uint32_t code) {
    const auto& table = foldTable();
    const auto it = std::lower_bound(
        table.begin(), table.end(), code,
        [](const std::pair<uint32_t, uint32_t>& entry, uint32_t value) {
            return entry.first < value;
        });
    return (it != table.end() && it->first == code) ? it->second : code;
}

const std::vector<uint32_t>& simpleCaseFoldCandidates(uint32_t folded) {
    static const std::vector<uint32_t> none;
    const auto& table = reverseFoldTable();
    const auto it = table.find(folded);
    return it == table.end() ? none : it->second;
}

const std::vector<uint32_t>& simpleCaseFoldSources() {
    static const std::vector<uint32_t> sources = [] {
        std::vector<uint32_t> out;
        out.reserve(data::kSimpleCaseFoldCount);
        for (const auto& entry : foldTable()) out.push_back(entry.first);
        return out;
    }();
    return sources;
}

}  // namespace bronze::regex
