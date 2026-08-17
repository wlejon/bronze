#include "regex/unicode.h"

#include <algorithm>
#include <map>
#include <string_view>

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

// ---- Script and Script_Extensions (UAX #24) ---------------------------------

// The script a spelling names, or `kScriptCount` for one that names none.
// 22.2.1 UnicodeMatchPropertyValue matches EXACTLY against the names and
// aliases PropertyValueAliases.txt lists, so this is a lookup in the generated
// alias table and never a normalization of what was written: `\p{Script=greek}`
// is a syntax error, not a sloppy `Greek`.
uint16_t scriptIndex(std::string_view alias) {
    const data::ScriptAlias* begin = data::kScriptAliases;
    const data::ScriptAlias* end = begin + data::kScriptAliasCount;
    const data::ScriptAlias* it =
        std::lower_bound(begin, end, alias, [](const data::ScriptAlias& entry, std::string_view a) {
            return std::string_view(entry.name) < a;
        });
    if (it == end || std::string_view(it->name) != alias) {
        return static_cast<uint16_t>(data::kScriptCount);
    }
    return it->script;
}

RangeList scriptRanges(uint16_t script) {
    RangeList list;
    for (uint32_t i = 0; i < data::kScriptRunCount; ++i) {
        if (data::kScriptRuns[i].script != script) continue;
        const uint32_t hi = i + 1 < data::kScriptRunCount ? data::kScriptRuns[i + 1].start - 1
                                                          : kMaxCodePoint;
        addRange(list, data::kScriptRuns[i].start, hi);
    }
    normalizeRanges(list);
    return list;
}

// Script_Extensions, assembled the way ScriptExtensions.txt is written: the
// Script set, minus every code point the file OVERRIDES, plus the overrides
// whose set names this script. Deriving it rather than carrying a second full
// table is what makes the file's own rule — "all code points not explicitly
// listed have as their value the corresponding Script property value" — a
// consequence here instead of a claim.
RangeList scriptExtensionRanges(uint16_t script) {
    RangeList overridden;
    RangeList named;
    for (uint32_t i = 0; i < data::kScxRangeCount; ++i) {
        const data::ScxRange& range = data::kScxRanges[i];
        addRange(overridden, range.first, range.last);
        for (uint32_t j = 0; j < range.count; ++j) {
            if (data::kScxScripts[range.set + j] != script) continue;
            addRange(named, range.first, range.last);
            break;
        }
    }
    normalizeRanges(overridden);
    normalizeRanges(named);
    RangeList out = subtractRanges(scriptRanges(script), overridden);
    out.insert(out.end(), named.begin(), named.end());
    normalizeRanges(out);
    return out;
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
    "bronze carries General_Category and Script: General_Category's 30 values by "
    "alias or long name (`\\p{Lu}`, `\\p{Uppercase_Letter}`, "
    "`\\p{General_Category=Lu}`), the unions `C L LC M N P S Z`, the binary "
    "properties `ASCII`, `Any` and `Assigned` which follow from the same table, "
    "and `Script` / `Script_Extensions` by either spelling (`\\p{Script=Greek}`, "
    "`\\p{scx=Grek}`)";

// 22.2.1's Table 67, in full. Written out rather than derived because it IS a
// list in the specification and not a slice of the UCD: each entry names a set
// whose members may be sequences, and the list changes only when an edition
// adds one.
bool isPropertyOfStrings(std::string_view value) {
    static constexpr std::string_view kNames[] = {
        "Basic_Emoji",
        "Emoji_Keycap_Sequence",
        "RGI_Emoji",
        "RGI_Emoji_Flag_Sequence",
        "RGI_Emoji_Modifier_Sequence",
        "RGI_Emoji_Tag_Sequence",
        "RGI_Emoji_ZWJ_Sequence",
    };
    for (std::string_view known : kNames) {
        if (value == known) return true;
    }
    return false;
}

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
    // Script first, and by name, because the two properties are one data file
    // and a value that names no script must not fall through to the
    // General_Category lookup — where `Greek` would find no category and be
    // reported as a misspelling rather than as the script it is.
    //
    // `sc` and `scx` differ in exactly one thing: whether a code point that is
    // COMMONLY used with a script but does not belong to it is a member. U+0342
    // COMBINING GREEK PERISPOMENI is Inherited by Script and Greek by
    // Script_Extensions, and a pattern that meant "Greek text" wants the second.
    if (name == "Script" || name == "sc" || name == "Script_Extensions" || name == "scx") {
        const uint16_t script = scriptIndex(value);
        if (script == data::kScriptCount) {
            error = quoted(name, value) +
                    " names no Unicode script. UAX #24's Script and Script_Extensions take a "
                    "value PropertyValueAliases.txt lists — a long name such as `Greek` or its "
                    "four-letter code `Grek` — matched exactly, since 22.2.1 is case sensitive";
            return false;
        }
        out = (name == "Script" || name == "sc") ? scriptRanges(script)
                                                 : scriptExtensionRanges(script);
        return true;
    }
    if (!name.empty() && name != "General_Category" && name != "gc") {
        error = "unsupported: " + quoted(name, value) + " — `" + std::string(name) +
                "` is not a property bronze implements as a name=value pair. " + kSupported;
        return false;
    }

    if (name.empty() && binaryPropertySet(value, out)) return true;

    const uint32_t mask = maskOfName(value);
    if (mask == 0) {
        // A SCRIPT written in the lone form is the one case that can be told
        // apart, and it is told apart: 22.2.1's lone `\p{...}` reads a
        // General_Category value or a binary property name and never a Script,
        // so `\p{Greek}` is a syntax error about the FORM rather than a missing
        // table — and saying which spelling would have worked is the whole of
        // the fix.
        if (name.empty() && scriptIndex(value) != data::kScriptCount) {
            error = quoted(name, value) + " is a Script value, and 22.2.1's lone `\\p{...}` "
                    "form reads only a General_Category value or a binary property name. "
                    "Write `\\p{Script=" + std::string(value) + "}` for the script, or "
                    "`\\p{Script_Extensions=" + std::string(value) + "}` for the characters "
                    "commonly used with it";
            return false;
        }
        // The properties of STRINGS, which are the one family of unknown
        // property bronze can name exactly — 22.2.1's Table 67 is a closed list
        // of seven, where the binary properties of Table 66 are a whole file of
        // UAX #44. What is missing is the DATA and no longer the representation:
        // a class set holds members that are sequences since `\q{...}` does, so
        // each of the seven needs its list of emoji sequences from UTS #51's
        // `emoji-sequences.txt` and `emoji-zwj-sequences.txt` — thousands of
        // them, and a generated table per property. Naming the family matters
        // because the generic message would send a reader off to check their
        // spelling of a name they spelled correctly.
        if (name.empty() && isPropertyOfStrings(value)) {
            error = "unsupported: " + quoted(name, value) +
                    " is a property of STRINGS (22.2.1's Table 67), which is legal only under "
                    "the `v` flag. bronze holds class members that are strings — `\\q{...}` "
                    "works — but carries no table for any of the seven, each of which is a list "
                    "of emoji sequences from UTS #51";
            return false;
        }
        // One message for a misspelling and for a real property with no table,
        // because bronze genuinely cannot tell those two apart — it has no list
        // of every UAX #44 property, and inventing one to sort them would be a
        // guess. What it can do is name the offender and say what it does have,
        // which is what an unknown `\p{...}` most needs.
        error = "unsupported: " + quoted(name, value) + " names no property value bronze "
                "carries. It is either a misspelling or a UAX #44 binary property — such as "
                "`Alphabetic` or `White_Space` — whose data bronze does not have. " +
                kSupported;
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

std::string_view scriptOf(uint32_t code) {
    const data::ScriptRun* begin = data::kScriptRuns;
    const data::ScriptRun* end = begin + data::kScriptRunCount;
    // The run CONTAINING `code`, which is the last one starting at or below it.
    // `kScriptRuns[0].start` is 0, so there is always one.
    const data::ScriptRun* it =
        std::upper_bound(begin, end, code, [](uint32_t value, const data::ScriptRun& run) {
            return value < run.start;
        });
    return data::kScriptNames[(it - 1)->script];
}

bool scriptExtensionsContain(uint32_t code, std::string_view alias) {
    const uint16_t script = scriptIndex(alias);
    if (script == data::kScriptCount) return false;
    for (uint32_t i = 0; i < data::kScxRangeCount; ++i) {
        const data::ScxRange& range = data::kScxRanges[i];
        if (code < range.first) break;  // sorted and disjoint
        if (code > range.last) continue;
        for (uint32_t j = 0; j < range.count; ++j) {
            if (data::kScxScripts[range.set + j] == script) return true;
        }
        return false;
    }
    // No override, so the set is the one-element set holding the Script.
    return scriptOf(code) == data::kScriptNames[script];
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

const RangeList& simpleCaseFoldFixedPoints() {
    static const RangeList fixed = [] {
        RangeList moved;
        for (uint32_t code : simpleCaseFoldSources()) addRange(moved, code, code);
        normalizeRanges(moved);
        return complementRanges(moved, kMaxCodePoint);
    }();
    return fixed;
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
