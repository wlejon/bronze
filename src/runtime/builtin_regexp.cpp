// The `RegExp` object: the constructor, the members a program reads off one,
// and `exec` — which every other regular-expression operation in bronze is
// built from.
//
// The pattern grammar and the matcher are `src/regex` and know nothing about
// values; what is here is the JavaScript surface over them. Three things it
// owns and they do not:
//
//  - the COMPILED-PATTERN TABLE. A `regex::Pattern` is a C++ tree the moving
//    collector must never touch, so it lives outside the heap and a RegExp
//    holds its index. Two regular expressions with the same source and flags
//    share one entry, which is what stops a literal inside a loop from
//    recompiling its pattern per iteration.
//  - `lastIndex`, and the `g`/`y` protocol around it (22.2.7.2).
//  - the MATCH ARRAY: an array of captures that also carries `index`, `input`
//    and `groups`, which is why arrays grew a named-property object.
//
// A RegExp has no prototype object, for the reason a Map has none: it carries
// no shape, so there is nothing to hang one on. Its methods are handed out by
// the property path, and `re instanceof RegExp` is false, which is a deliberate
// divergence from node.

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "regex/regex.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/number_format.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// Compiled patterns, and the memo that keeps one per (source, flags). Both
// outlive every RegExp that names them: a `regex::Pattern` is small, a program
// has finitely many distinct patterns, and freeing one would need a reference
// count on a table the collector cannot see into.
std::vector<regex::PatternPtr>& programs() {
    static std::vector<regex::PatternPtr> table;
    return table;
}

// Keyed on the flags and the source with a separator no flag letter can be, so
// `/ab/g` and `/abg/` cannot collide. A std::map and not a hash map because the
// project forbids hash-map iteration order in output paths and one table that
// is never iterated is not worth a second rule to remember.
std::map<std::string, uint32_t>& programIndex() {
    static std::map<std::string, uint32_t> table;
    return table;
}

regex::Units unitsOfString(const StringHeader* str) {
    const std::vector<uint16_t> units = rtStringUnits(str);
    return regex::Units(units.begin(), units.end());
}

// Compiles, or answers the index of an identical earlier compilation. Returns
// false with the exception cell set: 22.2.3.1 step 4 makes a pattern that does
// not parse a SyntaxError, and a pattern bronze refuses names itself in the
// same message (`src/regex` writes both).
bool programFor(const std::string& sourceUtf8, const regex::Units& source,
                const std::string& flagsText, regex::Flags& flags, uint32_t& out) {
    std::string error;
    if (!regex::parseFlags(flagsText, flags, error)) {
        rtThrowSyntaxError(error);
        return false;
    }
    // Keyed on the CANONICAL flags, so `/a/gi` and `/a/ig` are one
    // compilation: the flag letters are a set, and two spellings of the same
    // set describe the same pattern.
    const std::string key = flags.text() + "\n" + sourceUtf8;
    auto it = programIndex().find(key);
    if (it != programIndex().end()) {
        out = it->second;
        return true;
    }
    regex::PatternPtr pattern = regex::compile(source, flags, error);
    if (!pattern) {
        rtThrowSyntaxError("Invalid regular expression: /" + sourceUtf8 + "/" + flagsText + ": " +
                           error);
        return false;
    }
    out = static_cast<uint32_t>(programs().size());
    programs().push_back(std::move(pattern));
    programIndex().emplace(key, out);
    return true;
}

bool isRegExp(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == RegExpHeader::kFlags;
}

const regex::Pattern& programOf(Value re) {
    const auto* header = re.asObject<RegExpHeader>();
    const auto index = static_cast<size_t>(header->programIndex.asNumber());
    if (index >= programs().size()) fatal("internal: a RegExp with no compiled pattern");
    return *programs()[index];
}

// 22.2.6.10 EscapeRegExpPattern. `source` is the text a LITERAL of this
// pattern would carry, so it must be a pattern that can sit between two
// slashes on one line: the empty pattern is spelled `(?:)` (`//` is a line
// comment), and an unescaped `/` or line terminator — which only a pattern
// built from a string can contain — is escaped. Escaping happens before the
// compile, and every escape introduced here compiles back to the character it
// replaced, so the pattern the source describes is still the pattern that runs.
std::string escapeRegExpPattern(const std::string& pattern) {
    if (pattern.empty()) return "(?:)";
    std::string out;
    out.reserve(pattern.size());
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) {
            out.push_back(c);
            out.push_back(pattern[++i]);
            continue;
        }
        switch (c) {
            case '/': out += "\\/"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default:
                // U+2028 and U+2029 are line terminators too, and are the only
                // multi-byte ones; matching their UTF-8 bytes is exact because
                // no other code point encodes to a sequence containing them.
                if (i + 2 < pattern.size() && static_cast<unsigned char>(c) == 0xE2 &&
                    static_cast<unsigned char>(pattern[i + 1]) == 0x80 &&
                    (static_cast<unsigned char>(pattern[i + 2]) == 0xA8 ||
                     static_cast<unsigned char>(pattern[i + 2]) == 0xA9)) {
                    out += static_cast<unsigned char>(pattern[i + 2]) == 0xA8 ? "\\u2028"
                                                                             : "\\u2029";
                    i += 2;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

// 22.2.3.1: the source and the flags, compiled, with `lastIndex` at zero.
// `undefined` with the exception cell set when the pattern does not compile.
Value makeRegExp(Rooted<Value>& sourceStr, const std::string& flagsText) {
    const std::string written = rtUtf8Chars(sourceStr.get().asString<StringHeader>());
    const std::string sourceUtf8 = escapeRegExpPattern(written);
    if (sourceUtf8 != written) sourceStr.set(rtMakeString(sourceUtf8.c_str()));
    const regex::Units source = unitsOfString(sourceStr.get().asString<StringHeader>());
    uint32_t index = 0;
    regex::Flags flags;
    if (!programFor(sourceUtf8, source, flagsText, flags, index)) return Value::fromUndefined();

    // The flags are re-spelled in 22.2.6.5's order, so `/a/yg`.flags is "gy".
    Rooted<Value> canonicalFlags{rtMakeString(flags.text())};

    HeapObjectHeader* raw =
        rtHeap().allocate(sizeof(RegExpHeader) - sizeof(HeapObjectHeader), Tag::Object);
    auto* re = reinterpret_cast<RegExpHeader*>(raw);
    re->header.flags = RegExpHeader::kFlags;
    re->source = sourceStr.get();
    re->flagsText = canonicalFlags.get();
    re->lastIndex = Value::fromDouble(0.0);
    re->programIndex = Value::fromDouble(index);
    return Value::fromObject(re);
}

// ---- exec -------------------------------------------------------------------

// 22.2.7.2 steps 16-28: the captures as an array that also carries `index`,
// `input` and `groups`. The three named properties are created in the order
// the specification creates them, because that is the order they enumerate and
// print in.
Value buildMatchArray(const regex::Pattern& pattern, Rooted<Value>& inputStr,
                      const regex::MatchResult& match) {
    const std::vector<uint16_t> input = rtStringUnits(inputStr.get().asString<StringHeader>());
    const uint32_t groups = regex::captureCount(pattern);

    Rooted<Value> array{Value(bronze_create_array(groups + 1))};
    auto slice = [&](int64_t from, int64_t to) {
        std::vector<uint16_t> piece(input.begin() + static_cast<size_t>(from),
                                    input.begin() + static_cast<size_t>(to));
        return rtStringFromUnits(piece);
    };

    ObjectHeader* props = ArrayHeader::ensureProperties(rtHeap(), rtArena(), array);
    (void)props;
    auto defineNamed = [&](const char* name, Rooted<Value>& value) {
        Rooted<Value> propsRoot{array.get().asObject<ArrayHeader>()->properties};
        Rooted<Value> key{rtMakeString(name)};
        propsRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, value);
    };

    Rooted<Value> indexValue{Value::fromDouble(static_cast<double>(match.start()))};
    defineNamed("index", indexValue);
    defineNamed("input", inputStr);

    // `groups` is an object only when the pattern has a named group, and
    // `undefined` otherwise (22.2.7.2 step 8) — which is why an ordinary match
    // array prints `groups: undefined` rather than `groups: {}`.
    Rooted<Value> groupsObject;
    if (regex::hasNamedGroups(pattern)) {
        groupsObject.set(Value(bronze_create_object()));
    }
    defineNamed("groups", groupsObject);

    for (uint32_t i = 0; i <= groups; ++i) {
        const int64_t from = match.captures[static_cast<size_t>(i) * 2];
        const int64_t to = match.captures[static_cast<size_t>(i) * 2 + 1];
        Rooted<Value> element;
        if (from != regex::MatchResult::kUnset) element.set(slice(from, to));
        array.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, element);
        if (i == 0 || !groupsObject.get().isObject()) continue;
        const std::string& name = regex::groupName(pattern, i);
        if (name.empty()) continue;
        Rooted<Value> key{rtMakeString(name)};
        groupsObject.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, element);
    }
    return array.get();
}

// ToLength (7.1.20) of whatever a program assigned to `lastIndex`.
size_t lastIndexOf(Value re) {
    const double raw = re.asObject<RegExpHeader>()->lastIndex.asNumber();
    if (std::isnan(raw) || raw <= 0.0) return 0;
    if (raw >= 9007199254740991.0) return static_cast<size_t>(-1);
    return static_cast<size_t>(raw);
}

}  // namespace

bool rtIsRegExp(Value v) { return isRegExp(v); }

Value rtRegExpExec(Rooted<Value>& re, Rooted<Value>& inputStr) {
    if (!isRegExp(re.get())) {
        return rtThrowTypeError("RegExp.prototype.exec called on an incompatible receiver");
    }
    const regex::Pattern& pattern = programOf(re.get());
    const regex::Flags& flags = regex::patternFlags(pattern);
    const std::vector<uint16_t> raw = rtStringUnits(inputStr.get().asString<StringHeader>());
    const regex::Units input(raw.begin(), raw.end());

    // 22.2.7.2 step 6: a pattern with neither `g` nor `y` ignores `lastIndex`
    // entirely, which is what makes `/a/.exec(s)` idempotent and `/a/g.exec(s)`
    // a cursor.
    const bool tracksLastIndex = flags.global || flags.sticky;
    size_t from = tracksLastIndex ? lastIndexOf(re.get()) : 0;

    regex::MatchResult match;
    std::string error;
    regex::ExecStatus status = regex::ExecStatus::NoMatch;
    if (from <= input.size()) {
        status = flags.sticky ? regex::matchAt(pattern, input, from, match, error)
                              : regex::search(pattern, input, from, match, error);
    }
    if (status == regex::ExecStatus::Error) {
        // The matcher gave up rather than answering: a case fold bronze has no
        // table for, or a backtracking budget. Both are hard errors and not
        // catchable throws, because both mean bronze does not know the answer.
        fatal(error.c_str());
    }
    if (status != regex::ExecStatus::Match) {
        if (tracksLastIndex) {
            re.get().asObject<RegExpHeader>()->lastIndex = Value::fromDouble(0.0);
        }
        return Value::fromNull();
    }
    if (tracksLastIndex) {
        re.get().asObject<RegExpHeader>()->lastIndex =
            Value::fromDouble(static_cast<double>(match.end()));
    }
    return buildMatchArray(pattern, inputStr, match);
}

const regex::Pattern& rtRegExpPattern(Value re) { return programOf(re); }

Value rtRegExpBuildMatchArray(const regex::Pattern& pattern, Rooted<Value>& inputStr,
                              const regex::MatchResult& match) {
    return buildMatchArray(pattern, inputStr, match);
}

void rtRegExpSetLastIndex(Value re, double value) {
    re.asObject<RegExpHeader>()->lastIndex = Value::fromDouble(value);
}

double rtRegExpLastIndex(Value re) { return re.asObject<RegExpHeader>()->lastIndex.asNumber(); }

Value rtRegExpFromParts(Rooted<Value>& sourceStr, const std::string& flagsText) {
    return makeRegExp(sourceStr, flagsText);
}

namespace {

uint64_t regexpExec(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    Rooted<Value> input{rtValueToString(args[0])};
    return rtRegExpExec(self, input).rawBits();
}

// 22.2.6.16: `test` is `exec` and a null check. Written as exactly that, so
// the two can never disagree about `lastIndex`.
uint64_t regexpTest(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    Rooted<Value> input{rtValueToString(args[0])};
    Value result = rtRegExpExec(self, input);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return Value::fromBool(!result.isNull()).rawBits();
}

uint64_t regexpToString(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!isRegExp(self)) {
        return rtThrowTypeError("RegExp.prototype.toString called on an incompatible receiver")
            .rawBits();
    }
    const std::string text = rtRegExpText(self);
    return rtMakeString(text).rawBits();
}

// 22.2.3.1. The first argument may be a RegExp, in which case its source is
// reused and — when no flags argument is given — so are its flags.
uint64_t regexpConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> pattern{args[0]};
    std::string flagsText;
    if (isRegExp(pattern.get())) {
        Rooted<Value> inner{pattern.get().asObject<RegExpHeader>()->flagsText};
        flagsText = args.count() > 1 && !args[1].isUndefined()
                        ? rtUtf8Chars(rtValueToString(args[1]).asString<StringHeader>())
                        : rtUtf8Chars(inner.get().asString<StringHeader>());
        Rooted<Value> source{pattern.get().asObject<RegExpHeader>()->source};
        return makeRegExp(source, flagsText).rawBits();
    }
    // 22.2.3.1 step 5: no pattern argument is the EMPTY pattern, not the string
    // "undefined". What the empty pattern's source reads as is makeRegExp's.
    Rooted<Value> source{pattern.get().isUndefined() ? rtMakeString("")
                                                     : rtValueToString(pattern.get())};
    if (args.count() > 1 && !args[1].isUndefined()) {
        flagsText = rtUtf8Chars(rtValueToString(args[1]).asString<StringHeader>());
    }
    return makeRegExp(source, flagsText).rawBits();
}

struct RegExpMethod {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const RegExpMethod kRegExpMethods[] = {
    {"exec", regexpExec, 1},
    {"test", regexpTest, 1},
    {"toString", regexpToString, 0},
};

// RegExp.prototype, minus everything above and minus the flag accessors, which
// are real. A member ECMA-262 defines and bronze has not built is a named error
// rather than `undefined`.
//
// The SYMBOL-keyed members — `[Symbol.match]`, `[Symbol.replace]` and the rest
// of 22.2.6 — are not here and cannot be: this table is matched against a
// string, and no string names one of them. They are refused a step earlier, at
// `Symbol.match` itself, which is in builtin_symbol.cpp's unimplemented list.
const char* const kRegExpMembers[] = {
    "compile", "constructor", "hasIndices", "unicodeSets",
};

// The two groups of REAL members, as tables rather than as a ladder of `if`s,
// so that `rtRegExpMember` and `rtRegExpHasMember` read one list each. They are
// pointers-to-member rather than names alone because the reader needs the
// field, and a name list beside the ladder would be a second list to keep in
// step — which for `in` is exactly the failure mode being fixed elsewhere in
// this change.

// 22.2.6.10, 22.2.6.5 and 22.2.6.9: the three the header carries verbatim.
// `lastIndex` is the only own property of the three; the other two are
// prototype accessors, and `in` cannot tell the difference because a RegExp
// has no chain here for it to stop at.
struct HeaderMember {
    const char* name;
    Value RegExpHeader::* field;
};

const HeaderMember kRegExpHeaderMembers[] = {
    {"source", &RegExpHeader::source},
    {"flags", &RegExpHeader::flagsText},
    {"lastIndex", &RegExpHeader::lastIndex},
};

// The flag accessors of 22.2.6, each reading one bool out of the compiled
// pattern's flags.
struct FlagMember {
    const char* name;
    bool regex::Flags::* field;
};

const FlagMember kRegExpFlagMembers[] = {
    {"global", &regex::Flags::global},       {"ignoreCase", &regex::Flags::ignoreCase},
    {"multiline", &regex::Flags::multiline}, {"dotAll", &regex::Flags::dotAll},
    // 22.2.6.18: a real accessor now that the flag is a real mode, and the one
    // way a program can ask which alphabet a pattern was compiled over.
    {"unicode", &regex::Flags::unicode},     {"sticky", &regex::Flags::sticky},
};

}  // namespace

Value rtRegExpConstructor(const std::string& name) {
    if (name != "RegExp") return Value::fromUndefined();
    return rtNativeFunction(regexpConstructor, 2);
}

Value rtRegExpMethod(const std::string& key) {
    for (const RegExpMethod& m : kRegExpMethods) {
        if (key == m.name) return rtNativeFunction(m.code, m.arity);
    }
    return Value::fromUndefined();
}

std::string rtRegExpText(Value re) {
    const auto* header = re.asObject<RegExpHeader>();
    return "/" + rtUtf8Chars(header->source.asString<StringHeader>()) + "/" +
           rtUtf8Chars(header->flagsText.asString<StringHeader>());
}

Value rtRegExpMember(Value re, const std::string& key) {
    const auto* header = re.asObject<RegExpHeader>();
    for (const HeaderMember& m : kRegExpHeaderMembers) {
        if (key == m.name) return header->*m.field;
    }
    const regex::Flags& flags = regex::patternFlags(programOf(re));
    for (const FlagMember& m : kRegExpFlagMembers) {
        if (key == m.name) return Value::fromBool(flags.*m.field);
    }
    Value method = rtRegExpMethod(key);
    if (!method.isUndefined()) return method;
    rtCheckUnimplementedMember("RegExp.prototype", kRegExpMembers, std::size(kRegExpMembers), key);
    return Value::fromUndefined();
}

// The same three tables, asked whether the member EXISTS rather than what it
// is — which is all `in` needs, and is answerable for a member whose value
// bronze refuses to produce. A name in none of them is refused by name if
// ECMA-262 defines it, exactly as a read of it is, and is `false` only when the
// read path would answer `undefined`.
bool rtRegExpHasMember(const std::string& key) {
    for (const HeaderMember& m : kRegExpHeaderMembers) {
        if (key == m.name) return true;
    }
    for (const FlagMember& m : kRegExpFlagMembers) {
        if (key == m.name) return true;
    }
    for (const RegExpMethod& m : kRegExpMethods) {
        if (key == m.name) return true;
    }
    rtCheckUnimplementedMember("RegExp.prototype", kRegExpMembers, std::size(kRegExpMembers), key);
    return false;
}

bool rtRegExpSetMember(Value re, const std::string& key, Value value) {
    if (key != "lastIndex") return false;
    re.asObject<RegExpHeader>()->lastIndex = Value::fromDouble(rtToNumber(value));
    return true;
}

}  // namespace bronze::runtime
