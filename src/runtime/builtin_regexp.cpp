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
#include <cstring>
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
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// Compiled patterns, and the memo that keeps one per (source, flags). Both
// outlive every RegExp that names them: a `regex::Pattern` is small, a program
// has finitely many distinct patterns, and freeing one would need a reference
// count on a table the collector cannot see into.
std::vector<regex::PatternPtr>& programs() {
    static thread_local std::vector<regex::PatternPtr> table;
    return table;
}

// Keyed on the flags and the source with a separator no flag letter can be, so
// `/ab/g` and `/abg/` cannot collide. A std::map and not a hash map because the
// project forbids hash-map iteration order in output paths and one table that
// is never iterated is not worth a second rule to remember.
std::map<std::string, uint32_t>& programIndex() {
    static thread_local std::map<std::string, uint32_t> table;
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

// 22.2.7.8 MakeMatchIndicesIndexPairArray: the same captures as POSITIONS.
// Entry i is the two-element array `[start, end]` — half-open, like every other
// range in the language — or `undefined` for a group that did not participate,
// which is the same distinction `m[i] === undefined` draws one array over.
//
// It needs no input string and no slicing: the extents are what the matcher
// already recorded to cut the captures out of, so this reads the very numbers
// `buildMatchArray` throws away.
//
// `groups` here is OrdinaryObjectCreate(NULL) (step 4), which is why it is
// built from a null root shape rather than with `bronze_create_object`.
Value buildMatchIndices(const regex::Pattern& pattern, const regex::MatchResult& match) {
    const uint32_t groups = regex::captureCount(pattern);
    Rooted<Value> array{Value(bronze_create_array(groups + 1))};

    Rooted<Value> groupsObject;
    if (regex::hasNamedGroups(pattern)) {
        Rooted<Value> noPrototype{Value::fromNull()};
        Value fresh = Value::fromObject(ObjectHeader::create(
            rtHeap(), rtArena(), rtRootShapeForPrototype(noPrototype.get())));
        fresh.asObject<ObjectHeader>()->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
        groupsObject.set(fresh);
    }

    // Step 5 puts `groups` on the array BEFORE the pairs, so it is the first
    // named property and the indices are the elements — the same shape the
    // match array itself has, and the order both enumerate in.
    ArrayHeader::ensureProperties(rtHeap(), rtArena(), array);
    {
        Rooted<Value> propsRoot{array.get().asObject<ArrayHeader>()->properties};
        Rooted<Value> key{rtMakeString("groups")};
        propsRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, groupsObject);
    }

    for (uint32_t i = 0; i <= groups; ++i) {
        const int64_t from = match.captures[static_cast<size_t>(i) * 2];
        const int64_t to = match.captures[static_cast<size_t>(i) * 2 + 1];
        Rooted<Value> pair;
        if (from != regex::MatchResult::kUnset) {
            pair.set(Value(bronze_create_array(2)));
            auto* header = pair.get().asObject<ArrayHeader>();
            Rooted<Value> start{Value::fromDouble(static_cast<double>(from))};
            header->setElem(rtHeap(), 0, start);
            Rooted<Value> end{Value::fromDouble(static_cast<double>(to))};
            pair.get().asObject<ArrayHeader>()->setElem(rtHeap(), 1, end);
        }
        array.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, pair);
        if (i == 0 || !groupsObject.get().isObject()) continue;
        const std::string& name = regex::groupName(pattern, i);
        if (name.empty()) continue;
        Rooted<Value> key{rtMakeString(name)};
        groupsObject.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, pair);
    }
    return array.get();
}

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

    // Step 35, and last for the reason the other three are in their order: the
    // pair array is built from the captures the loop above has just walked, so
    // `indices` is the final named property and prints after `groups`.
    if (regex::patternFlags(pattern).hasIndices) {
        Rooted<Value> indices{buildMatchIndices(pattern, match)};
        defineNamed("indices", indices);
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

// ---- 22.2.5.2 RegExp.escape -------------------------------------------------

// SyntaxCharacter :: one of ^ $ \ . * + ? ( ) [ ] { } | — the characters that
// mean something to the pattern grammar and so must be backslashed to mean
// themselves.
bool isSyntaxCharacter(uint32_t cp) {
    switch (cp) {
        case '^': case '$': case '\\': case '.': case '*': case '+': case '?':
        case '(': case ')': case '[': case ']': case '{': case '}': case '|':
            return true;
        default:
            return false;
    }
}

void appendHex(std::string& out, uint32_t value, size_t width) {
    static const char kDigits[] = "0123456789abcdef";
    std::string digits;
    do {
        digits.insert(digits.begin(), kDigits[value & 0xF]);
        value >>= 4;
    } while (value != 0);
    while (digits.size() < width) digits.insert(digits.begin(), '0');
    out += digits;
}

// 22.2.5.2.1 EncodeForRegExpEscape. Three tiers: a syntax character (and `/`,
// which a LITERAL would otherwise end) takes a backslash; the five control
// characters take their named escape; and anything a reader could mistake for
// punctuation, whitespace, or a lone surrogate takes a numeric escape.
//
// The third tier is what makes the member worth having over a hand-rolled
// `replace`: `escape("a b")` is "a\x20b", so the result stays safe to paste
// into an `x`-flagged pattern or a string built by concatenation.
void encodeForRegExpEscape(std::string& out, uint32_t cp) {
    if (isSyntaxCharacter(cp) || cp == '/') {
        out.push_back('\\');
        // Every syntax character is ASCII, so one byte is the whole encoding.
        out.push_back(static_cast<char>(cp));
        return;
    }
    switch (cp) {
        case 0x09: out += "\\t"; return;
        case 0x0A: out += "\\n"; return;
        case 0x0B: out += "\\v"; return;
        case 0x0C: out += "\\f"; return;
        case 0x0D: out += "\\r"; return;
        default: break;
    }
    // Step 3's otherPunctuators, plus WhiteSpace, plus LineTerminator, plus a
    // lone surrogate — a code point that must not be left bare.
    static const char* const kOtherPunctuators = ",-=<>#&!%:;@~'`\"";
    const bool punctuator = cp < 0x80 && std::strchr(kOtherPunctuators, static_cast<char>(cp)) &&
                            cp != 0;
    const bool space = cp == 0x20 || cp == 0xA0 || cp == 0x1680 ||
                       (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F ||
                       cp == 0x3000 || cp == 0xFEFF;
    const bool lineTerminator = cp == 0x2028 || cp == 0x2029;
    const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
    if (punctuator || space || lineTerminator || surrogate) {
        if (cp <= 0xFF) {
            out += "\\x";
            appendHex(out, cp, 2);
            return;
        }
        out += "\\u{";
        appendHex(out, cp, 1);
        out += "}";
        return;
    }
    // Anything else is itself. Written back as UTF-8, which is the encoding
    // `rtMakeString` reads.
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// 22.2.5.2 RegExp.escape(S). A non-string argument is a TypeError and NOT
// ToString'd (step 1), which is the member deliberately refusing to guess: the
// whole point of it is that the result is safe to interpolate, and a number
// silently stringified is how a caller ends up escaping the wrong thing.
uint64_t regexpEscape(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!args[0].isString()) {
        return rtThrowTypeError("RegExp.escape requires a string argument").rawBits();
    }
    Rooted<Value> input{args[0]};
    const std::vector<uint16_t> units = rtStringUnits(input.get().asString<StringHeader>());
    std::string out;
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t cp = units[i];
        // StringToCodePoints: a well-formed pair is ONE code point, which is
        // what keeps an astral character out of the lone-surrogate escape above.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
            units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            ++i;
        }
        // Step 3.a: the FIRST code point takes a hex escape when it is a digit
        // or an ASCII letter, so the result can never start a flag or read as
        // part of an identifier at the splice point. Only the first — `escape`
        // of "ab" is "\x61b".
        const bool first = out.empty();
        const bool alnum = (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') ||
                           (cp >= 'A' && cp <= 'Z');
        if (first && alnum) {
            out += "\\x";
            appendHex(out, cp, 2);
            continue;
        }
        encodeForRegExpEscape(out, cp);
    }
    return rtMakeString(out).rawBits();
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
// string, and no string names one of them. They live in
// builtin_regexp_symbols.cpp and are answered by key, through
// `rtRegExpSymbolMethod`, from the symbol-keyed read path.
const char* const kRegExpMembers[] = {
    "compile", "constructor",
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
    // 22.2.6.6: `d` decides whether `exec` attaches `indices`, and this is how
    // a program asks which it will get without running a match.
    {"hasIndices", &regex::Flags::hasIndices},
    {"global", &regex::Flags::global},       {"ignoreCase", &regex::Flags::ignoreCase},
    {"multiline", &regex::Flags::multiline}, {"dotAll", &regex::Flags::dotAll},
    // 22.2.6.18: a real accessor now that the flag is a real mode, and the one
    // way a program can ask which alphabet a pattern was compiled over.
    {"unicode", &regex::Flags::unicode},
    // 22.2.6.19: the second reading of the same mode, which is why it is a bit
    // of its own and not `unicode` again — a program can tell `/a/u` from `/a/v`.
    {"unicodeSets", &regex::Flags::unicodeSets},
    {"sticky", &regex::Flags::sticky},
};

}  // namespace

Value rtRegExpConstructor(const std::string& name) {
    if (name != "RegExp") return Value::fromUndefined();
    return rtNativeFunction(regexpConstructor, 2);
}

// Which function object is %RegExp%, asked WITHOUT building it. The obvious
// spelling — materialise `RegExp` through `rtRegExpConstructor` and compare
// addresses — makes a question an allocation, and `rtNativeFunction` interns on
// first use, so the very first caller collects. That is not a cost, it is a
// correctness bug for the caller: this predicate is one rung of the
// function-object miss ladder in rt_prop.cpp, and a collection there retires the
// property box the rungs on either side of it are holding. The code pointer is
// the identity the intern table itself keys on, so comparing it answers the same
// question and allocates nothing — which is how `rtIsArrayConstructor` and
// `rtIsPromiseConstructor` have always answered theirs.
bool rtIsRegExpConstructor(Value fn) {
    return fn.isObject() && fn.asObject<HeapObjectHeader>()->flags == HeapKind::Function &&
           fn.asObject<FunctionHeader>()->code == regexpConstructor;
}

// The own members of the `RegExp` constructor FUNCTION object — today just
// `escape` (22.2.5.2). Answered from a table beside the value for the reason
// `rtMapStatic` is: the constructor is an interned function singleton with no
// property object of its own, so the property path asks this instead of
// walking a chain that does not exist.
//
// Both guards come before the only allocation, so a receiver that is not
// %RegExp% — which is every receiver, on almost every property miss in the
// program — costs two comparisons and nothing else.
bool rtRegExpStatic(Value fn, const std::string& key, Value& out) {
    if (!rtIsRegExpConstructor(fn)) return false;
    if (key != "escape") return false;
    out = rtNativeFunction(regexpEscape, 1);
    return true;
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
    // `re.lastIndex = {valueOf(){...}}` runs 7.1.4 on the value, which is user
    // code: the receiver is rooted across it and the header taken afterwards.
    Rooted<Value> reRoot{re};
    const double num = rtToNumber(value);
    if (rtExceptionPending()) return true;
    reRoot.get().asObject<RegExpHeader>()->lastIndex = Value::fromDouble(num);
    return true;
}

}  // namespace bronze::runtime
