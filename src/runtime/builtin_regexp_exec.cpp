// `RegExp.prototype.exec` (ECMA-262 22.2.6.2) and everything it stands on:
// the compiled-pattern table, the match array, and the `lastIndex` protocol
// of 22.2.7.2. Every other regular-expression operation in bronze — `test`,
// the five symbol-keyed members, the string members that call them — is built
// from what is here.
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

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "regex/regex.h"
#include "runtime/array.h"
#include "runtime/builtin_regexp_internal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/integrity.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
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

// 22.2.7.2 steps 1-2: ToLength (7.1.20) of whatever the program assigned to
// `lastIndex`. The slot holds the assigned VALUE — a string stays a string —
// so this may run user code (an object's `valueOf`), and the RegExp is read
// through its root afterwards. `ok` is false with the exception pending.
size_t lastIndexOf(Rooted<Value>& re, bool& ok) {
    ok = true;
    const Value stored = re.get().asObject<RegExpHeader>()->lastIndex;
    double raw = 0.0;
    if (stored.isNumber()) {
        raw = stored.asNumber();
    } else {
        raw = rtToNumber(stored);
        if (rtExceptionPending()) {
            ok = false;
            return 0;
        }
    }
    if (std::isnan(raw) || raw <= 0.0) return 0;
    if (raw >= 9007199254740991.0) return static_cast<size_t>(-1);
    return static_cast<size_t>(raw);
}

// 22.2.7.2 step 15 / 22.2.7.1: the `lastIndex` write is Set(R, "lastIndex",
// v, true), a TypeError when the property is not writable — which a frozen
// RegExp's is not.
bool storeLastIndex(Rooted<Value>& re, double value) {
    if (rtIntegrityLevel(re.get()) == IntegrityLevel::Frozen) {
        rtThrowTypeError("Cannot assign to read only property 'lastIndex' of a frozen RegExp");
        return false;
    }
    re.get().asObject<RegExpHeader>()->lastIndex = Value::fromDouble(value);
    return true;
}

}  // namespace

bool rtInitializeRegExp(Rooted<Value>& re, Rooted<Value>& sourceStr, const std::string& flagsText) {
    const std::string written = rtUtf8Chars(sourceStr.get().asString<StringHeader>());
    const std::string sourceUtf8 = escapeRegExpPattern(written);
    if (sourceUtf8 != written) sourceStr.set(rtMakeString(sourceUtf8.c_str()));
    const regex::Units source = unitsOfString(sourceStr.get().asString<StringHeader>());
    uint32_t index = 0;
    regex::Flags flags;
    if (!programFor(sourceUtf8, source, flagsText, flags, index)) return false;

    // The flags are re-spelled in 22.2.6.4's order, so `/a/yg`.flags is "gy".
    Rooted<Value> canonicalFlags{rtMakeString(flags.text())};

    auto* header = re.get().asObject<RegExpHeader>();
    header->source = sourceStr.get();
    header->flagsText = canonicalFlags.get();
    header->lastIndex = Value::fromDouble(0.0);
    header->programIndex = Value::fromDouble(index);
    return true;
}

Value rtMakeRegExp(Rooted<Value>& sourceStr, const std::string& flagsText) {
    Rooted<Value> re{rtAllocateRegExp(rtRegExpInstanceShape())};
    if (!rtInitializeRegExp(re, sourceStr, flagsText)) return Value::fromUndefined();
    return re.get();
}

Value rtAllocateRegExp(Shape* shape) {
    ObjectHeader* obj = ObjectHeader::createWithInternalSlots(rtHeap(), rtArena(), shape,
                                                              RegExpHeader::kInternalSlots);
    obj->header.flags = RegExpHeader::kFlags;
    // The four internal slots came back `undefined`, which is exactly the
    // "allocated, not yet initialised" state 22.2.3.2 RegExpAlloc leaves a
    // derived constructor's `this` in until `super()` runs.
    return Value::fromObject(obj);
}

bool rtIsRegExp(Value v) { return isRegExp(v); }

Value rtRegExpExec(Rooted<Value>& re, Rooted<Value>& inputStr) {
    if (!isRegExp(re.get()) || !re.get().asObject<RegExpHeader>()->programIndex.isNumber()) {
        return rtThrowTypeError("RegExp.prototype.exec called on an incompatible receiver");
    }
    // 22.2.7.2 step 6: a pattern with neither `g` nor `y` ignores `lastIndex`
    // entirely, which is what makes `/a/.exec(s)` idempotent and `/a/g.exec(s)`
    // a cursor. Read FIRST: it can run user code, and every raw pointer below
    // is taken after it.
    const bool tracksLastIndex = [&] {
        const regex::Flags& f = regex::patternFlags(programOf(re.get()));
        return f.global || f.sticky;
    }();
    size_t from = 0;
    if (tracksLastIndex) {
        bool ok = true;
        from = lastIndexOf(re, ok);
        if (!ok) return Value::fromUndefined();
    }
    const regex::Pattern& pattern = programOf(re.get());
    const regex::Flags& flags = regex::patternFlags(pattern);
    const std::vector<uint16_t> raw = rtStringUnits(inputStr.get().asString<StringHeader>());
    const regex::Units input(raw.begin(), raw.end());

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
        if (tracksLastIndex && !storeLastIndex(re, 0.0)) return Value::fromUndefined();
        return Value::fromNull();
    }
    if (tracksLastIndex && !storeLastIndex(re, static_cast<double>(match.end()))) {
        return Value::fromUndefined();
    }
    return buildMatchArray(pattern, inputStr, match);
}

const regex::Pattern& rtRegExpPattern(Value re) { return programOf(re); }

Value rtRegExpBuildMatchArray(const regex::Pattern& pattern, Rooted<Value>& inputStr,
                              const regex::MatchResult& match) {
    return buildMatchArray(pattern, inputStr, match);
}

bool rtRegExpSetLastIndex(Rooted<Value>& re, double value) { return storeLastIndex(re, value); }

Value rtRegExpLastIndexValue(Value re) { return re.asObject<RegExpHeader>()->lastIndex; }

void rtRegExpRestoreLastIndex(Value re, Value saved) {
    re.asObject<RegExpHeader>()->lastIndex = saved;
}

double rtRegExpLastIndexLength(Rooted<Value>& re, bool& ok) {
    const size_t n = lastIndexOf(re, ok);
    return n == static_cast<size_t>(-1) ? 9007199254740991.0 : static_cast<double>(n);
}

Value rtRegExpFromParts(Rooted<Value>& sourceStr, const std::string& flagsText) {
    return rtMakeRegExp(sourceStr, flagsText);
}

uint64_t rtRegExpExecBody(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    // 22.2.6.2 step 3 before step 4: the receiver is checked before the
    // argument is converted, and ToString can run user code.
    if (!isRegExp(self.get()) || !self.get().asObject<RegExpHeader>()->programIndex.isNumber()) {
        return rtThrowTypeError("RegExp.prototype.exec called on an incompatible receiver")
            .rawBits();
    }
    Rooted<Value> input{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtRegExpExec(self, input).rawBits();
}

}  // namespace bronze::runtime
