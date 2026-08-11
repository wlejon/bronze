// `String.prototype`'s members that take a PATTERN: `match`, `matchAll`,
// `replace`, `replaceAll`, `search` and `split`, and the `$`-substitution and
// function-replacer machinery all of them share.
//
// Separate from builtin_string.cpp because these are the only string members
// that know what a RegExp is, and because their argument is not a string: each
// of them accepts either a RegExp or a string that is matched literally, and
// the two readings differ in every detail that matters (`"a.c".split(".")`
// splits on a dot, `"a.c".split(/./)` splits on everything).
//
// Every one of them drives the matcher through `rtRegExpExec` or through the
// same `lastIndex` protocol it implements, so there is exactly one answer to
// "where does the next match start" (docs/0024 decision 5).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "regex/regex.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

using Units = std::vector<uint16_t>;

Units unitsOf(Value str) { return rtStringUnits(str.asString<StringHeader>()); }

Value stringOf(const Units& units) { return rtStringFromUnits(units); }

Units slice(const Units& units, size_t from, size_t to) {
    if (from > units.size()) from = units.size();
    if (to > units.size()) to = units.size();
    if (to < from) to = from;
    return Units(units.begin() + static_cast<std::ptrdiff_t>(from),
                 units.begin() + static_cast<std::ptrdiff_t>(to));
}

// The receiver as a string, or a TypeError. Every member here is defined on
// `String.prototype`, so a non-string `this` is 22.1.3's RequireObjectCoercible
// failure and not something to compute over.
bool requireString(Value self, const char* method, Value& out) {
    if (self.isString()) {
        out = self;
        return true;
    }
    rtThrowTypeError(std::string("String.prototype.") + method +
                     " called on a value that is not a string");
    return false;
}

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == 2;
}

// The pattern argument as a RegExp. A string argument is matched LITERALLY,
// which is what 22.1.3.19's `replace` does — it searches for the string, it
// does not compile it. So the string case is kept as a string throughout and
// only a RegExp argument reaches the matcher.
bool argumentIsRegExp(Value v) { return rtIsRegExp(v); }

// 22.1.3.19.1 GetSubstitution: `$$`, `$&`, `` $` ``, `$'`, `$1`..`$99` and
// `$<name>`. A `$` followed by anything else is a literal `$`, which is the
// specification's rule and not a fallback — `"a".replace("a", "$x")` is "$x".
Units substitute(const Units& replacement, const Units& input, size_t matchStart,
                 size_t matchEnd, const std::vector<Units>& captures,
                 const std::vector<bool>& capturePresent,
                 const std::vector<std::string>& captureNames) {
    Units out;
    for (size_t i = 0; i < replacement.size(); ++i) {
        if (replacement[i] != '$' || i + 1 == replacement.size()) {
            out.push_back(replacement[i]);
            continue;
        }
        const uint16_t next = replacement[i + 1];
        if (next == '$') {
            out.push_back('$');
            ++i;
            continue;
        }
        if (next == '&') {
            const Units whole = slice(input, matchStart, matchEnd);
            out.insert(out.end(), whole.begin(), whole.end());
            ++i;
            continue;
        }
        if (next == '`') {
            const Units before = slice(input, 0, matchStart);
            out.insert(out.end(), before.begin(), before.end());
            ++i;
            continue;
        }
        if (next == '\'') {
            const Units after = slice(input, matchEnd, input.size());
            out.insert(out.end(), after.begin(), after.end());
            ++i;
            continue;
        }
        if (next == '<' && !captureNames.empty()) {
            size_t close = i + 2;
            std::string name;
            while (close < replacement.size() && replacement[close] != '>') {
                name.push_back(static_cast<char>(replacement[close]));
                ++close;
            }
            if (close < replacement.size()) {
                i = close;
                for (size_t g = 0; g < captureNames.size(); ++g) {
                    if (captureNames[g] != name) continue;
                    if (capturePresent[g]) {
                        out.insert(out.end(), captures[g].begin(), captures[g].end());
                    }
                    break;
                }
                continue;
            }
            // No `>`: the `$<` is literal text, which is what the grammar
            // says when the production does not complete.
            out.push_back('$');
            continue;
        }
        if (next >= '0' && next <= '9') {
            // Two digits when they name a group that exists, one otherwise:
            // `$12` is group 12 in a pattern with twelve groups and group 1
            // followed by "2" in a pattern with one.
            size_t index = static_cast<size_t>(next - '0');
            size_t consumed = 1;
            if (i + 2 < replacement.size() && replacement[i + 2] >= '0' &&
                replacement[i + 2] <= '9') {
                const size_t twoDigit = index * 10 + static_cast<size_t>(replacement[i + 2] - '0');
                if (twoDigit >= 1 && twoDigit <= captures.size()) {
                    index = twoDigit;
                    consumed = 2;
                }
            }
            if (index >= 1 && index <= captures.size()) {
                if (capturePresent[index - 1]) {
                    out.insert(out.end(), captures[index - 1].begin(), captures[index - 1].end());
                }
                i += consumed;
                continue;
            }
        }
        out.push_back('$');
    }
    return out;
}

// One match, reduced to what a replacement needs. Holding the pieces rather
// than a match array is what lets `replace` with a `g` pattern run over a long
// string without allocating an array per match.
struct MatchPieces {
    size_t start = 0;
    size_t end = 0;
    std::vector<Units> captures;
    std::vector<bool> present;
    std::vector<std::string> names;
};

MatchPieces piecesOf(const regex::Pattern& pattern, const Units& input,
                     const regex::MatchResult& match) {
    MatchPieces out;
    out.start = static_cast<size_t>(match.start());
    out.end = static_cast<size_t>(match.end());
    const uint32_t groups = regex::captureCount(pattern);
    for (uint32_t g = 1; g <= groups; ++g) {
        const int64_t from = match.captures[static_cast<size_t>(g) * 2];
        const int64_t to = match.captures[static_cast<size_t>(g) * 2 + 1];
        const bool present = from != regex::MatchResult::kUnset;
        out.present.push_back(present);
        out.captures.push_back(present ? slice(input, static_cast<size_t>(from),
                                               static_cast<size_t>(to))
                                       : Units{});
        out.names.push_back(regex::groupName(pattern, g));
    }
    if (!regex::hasNamedGroups(pattern)) out.names.clear();
    return out;
}

// The matcher, run without the exception machinery: a failure here is bronze
// not knowing the answer, which docs/0020 decision 6 keeps a hard error.
regex::ExecStatus runMatch(const regex::Pattern& pattern, const regex::Units& input, size_t from,
                           bool sticky, regex::MatchResult& match) {
    std::string error;
    const regex::ExecStatus status =
        sticky ? regex::matchAt(pattern, input, from, match, error)
               : regex::search(pattern, input, from, match, error);
    if (status == regex::ExecStatus::Error) fatal(error.c_str());
    return status;
}

regex::Units toRegexUnits(const Units& units) { return regex::Units(units.begin(), units.end()); }

// ---- search -----------------------------------------------------------------

uint64_t stringSearch(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!requireString(Value(thisBits), "search", selfVal)) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> self{selfVal};
    const Units input = unitsOf(self.get());

    if (!argumentIsRegExp(args[0])) {
        // 22.1.3.16 creates a RegExp from the argument, so a string separator
        // is PATTERN TEXT here — unlike `replace` and `split`, where a string
        // is matched literally. `"a.c".search(".")` is 0.
        Rooted<Value> source{rtValueToString(args[0])};
        Rooted<Value> made{rtRegExpFromParts(source, "")};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        regex::MatchResult match;
        const regex::Pattern& pattern = rtRegExpPattern(made.get());
        if (runMatch(pattern, toRegexUnits(input), 0, false, match) != regex::ExecStatus::Match) {
            return Value::fromDouble(-1.0).rawBits();
        }
        return Value::fromDouble(static_cast<double>(match.start())).rawBits();
    }

    Rooted<Value> re{args[0]};
    // 22.1.3.16 steps 4-8: `search` saves and restores `lastIndex`, so it is
    // the one pattern member with no effect on the cursor.
    const double saved = rtRegExpLastIndex(re.get());
    regex::MatchResult match;
    const regex::Pattern& pattern = rtRegExpPattern(re.get());
    const regex::ExecStatus status = runMatch(pattern, toRegexUnits(input), 0, false, match);
    rtRegExpSetLastIndex(re.get(), saved);
    if (status != regex::ExecStatus::Match) return Value::fromDouble(-1.0).rawBits();
    return Value::fromDouble(static_cast<double>(match.start())).rawBits();
}

// ---- match / matchAll -------------------------------------------------------

// The RegExp a pattern argument denotes, made if the argument was not one.
// `extraFlags` is what `matchAll` needs: 22.1.3.14 requires a `g` pattern, and
// a bare string argument becomes one.
Value patternArgument(Value arg, const char* extraFlags) {
    if (rtIsRegExp(arg)) return arg;
    Rooted<Value> source{arg.isUndefined() ? rtMakeString("") : rtValueToString(arg)};
    return rtRegExpFromParts(source, extraFlags);
}

uint64_t stringMatch(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!requireString(Value(thisBits), "match", selfVal)) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> self{selfVal};
    Rooted<Value> re{patternArgument(args[0], "")};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    const regex::Flags& flags = regex::patternFlags(rtRegExpPattern(re.get()));
    // 22.1.3.13 step 3: without `g`, `match` IS `exec` — the same match array,
    // captures and all. With `g` it is the list of matched TEXTS and nothing
    // else, which is why the two answers have different shapes.
    if (!flags.global) return rtRegExpExec(re, self).rawBits();

    rtRegExpSetLastIndex(re.get(), 0.0);
    // Length ZERO, grown by the appends below: `bronze_create_array(n)` sets
    // the length, so passing a capacity guess would leave trailing `undefined`
    // elements in the result.
    Rooted<Value> out{Value(bronze_create_array(0))};
    uint32_t count = 0;
    for (;;) {
        Value result = rtRegExpExec(re, self);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (result.isNull()) break;
        Rooted<Value> matched{result.asObject<ArrayHeader>()->getElem(0)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), count++, matched);
        // An empty match would leave `lastIndex` where it is and loop for
        // ever; 22.2.6.8 step 8.f.iii advances it by one.
        if (matched.get().asString<StringHeader>()->getLength() == 0) {
            rtRegExpSetLastIndex(re.get(), rtRegExpLastIndex(re.get()) + 1);
        }
    }
    // 22.1.3.13 step 5.g: `null`, not an empty array, when nothing matched.
    if (count == 0) return Value::fromNull().rawBits();
    return out.get().rawBits();
}

// The iterator `matchAll` hands back. A plain object with `next` and
// `@@iterator`, exactly as a Map's iterators are (docs/0021 decision 1) — the
// state is three internal slots whose `@@` names keep them out of
// `Object.keys` and out of `console.log`.
StringHeader* internKey(const char* text) {
    StringHeader* tmp = StringHeader::createFromUTF8(rtHeap(), std::string_view(text));
    return StringHeader::internToArena(rtArena(), tmp);
}

StringHeader* keyRegExp() {
    static StringHeader* k = internKey("@@matchAllRegExp");
    return k;
}
StringHeader* keyInput() {
    static StringHeader* k = internKey("@@matchAllInput");
    return k;
}
StringHeader* keyDone() {
    static StringHeader* k = internKey("@@matchAllDone");
    return k;
}

Value readSlot(Rooted<Value>& obj, StringHeader* name) {
    Rooted<Value> key{Value::fromString(name)};
    return obj.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
}

void writeSlot(Rooted<Value>& obj, StringHeader* name, Rooted<Value>& val) {
    Rooted<Value> key{Value::fromString(name)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
}

Value iterResult(Rooted<Value>& value, bool done) {
    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> vk{rtMakeString("value")};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), vk, value);
    Rooted<Value> dk{rtMakeString("done")};
    Rooted<Value> dv{Value::fromBool(done)};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), dk, dv);
    return out.get();
}

uint64_t iterSelf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) { return thisBits; }

uint64_t matchAllNext(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    Rooted<Value> none;
    if (!self.get().isObject() ||
        self.get().asObject<HeapObjectHeader>()->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        return rtThrowTypeError("next called on an incompatible receiver").rawBits();
    }
    if (readSlot(self, keyDone()).asBool()) return iterResult(none, true).rawBits();

    Rooted<Value> re{readSlot(self, keyRegExp())};
    Rooted<Value> input{readSlot(self, keyInput())};
    Value result = rtRegExpExec(re, input);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    if (result.isNull()) {
        Rooted<Value> flag{Value::fromBool(true)};
        writeSlot(self, keyDone(), flag);
        return iterResult(none, true).rawBits();
    }
    Rooted<Value> match{result};
    // 22.2.9.2.1 step 8.e.iii: an empty match advances the cursor by one, or
    // the iterator would yield it for ever.
    Value first = match.get().asObject<ArrayHeader>()->getElem(0);
    if (first.isString() && first.asString<StringHeader>()->getLength() == 0) {
        rtRegExpSetLastIndex(re.get(), rtRegExpLastIndex(re.get()) + 1);
    }
    return iterResult(match, false).rawBits();
}

uint64_t stringMatchAll(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!requireString(Value(thisBits), "matchAll", selfVal)) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> self{selfVal};
    if (rtIsRegExp(args[0]) && !regex::patternFlags(rtRegExpPattern(args[0])).global) {
        // 22.1.3.14 step 2.b: a non-global RegExp is a TypeError rather than a
        // one-element iterator, because the method's whole contract is "all of
        // them" and a non-global pattern cannot deliver that.
        return rtThrowTypeError(
                   "String.prototype.matchAll called with a non-global RegExp argument")
            .rawBits();
    }
    // A COPY of the argument, so the iterator's cursor is its own: 22.1.3.14
    // step 3 clones the RegExp, and without that a `for-of` would move the
    // caller's `lastIndex`.
    Rooted<Value> source;
    std::string flagsText = "g";
    if (rtIsRegExp(args[0])) {
        Rooted<Value> original{args[0]};
        source.set(rtRegExpMember(original.get(), "source"));
        flagsText = rtUtf8Chars(rtRegExpMember(original.get(), "flags").asString<StringHeader>());
    } else {
        source.set(args[0].isUndefined() ? rtMakeString("") : rtValueToString(args[0]));
    }
    Rooted<Value> re{rtRegExpFromParts(source, flagsText)};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    static Shape* shape = nullptr;
    if (!shape) shape = rtNewRootShape(Value::fromUndefined());
    Rooted<Value> it{Value::fromObject(ObjectHeader::create(rtHeap(), rtArena(), shape))};
    it.get().asObject<ObjectHeader>()->header.flags = 0;
    Rooted<Value> nextFn{Value(bronze_function_singleton(matchAllNext, 0))};
    Rooted<Value> nk{rtMakeString("next")};
    it.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), nk, nextFn);
    Rooted<Value> selfFn{Value(bronze_function_singleton(iterSelf, 0))};
    writeSlot(it, rtIteratorKey(), selfFn);
    writeSlot(it, keyRegExp(), re);
    writeSlot(it, keyInput(), self);
    Rooted<Value> notDone{Value::fromBool(false)};
    writeSlot(it, keyDone(), notDone);
    return it.get().rawBits();
}

// ---- replace / replaceAll ---------------------------------------------------

// The replacer function's arguments (22.1.3.19 step 14.g): the matched text,
// then every capture, then the offset, then the whole input, then the groups
// object when the pattern has named groups.
Value callReplacer(Rooted<Value>& fn, const MatchPieces& pieces, const Units& input) {
    Rooted<Value> matched{stringOf(slice(input, pieces.start, pieces.end))};
    std::vector<Rooted<Value>> roots;
    roots.reserve(pieces.captures.size());
    for (size_t g = 0; g < pieces.captures.size(); ++g) {
        roots.emplace_back(pieces.present[g] ? stringOf(pieces.captures[g])
                                             : Value::fromUndefined());
    }
    Rooted<Value> whole{stringOf(input)};
    Rooted<Value> groups;
    if (!pieces.names.empty()) {
        groups.set(Value(bronze_create_object()));
        for (size_t g = 0; g < pieces.names.size(); ++g) {
            if (pieces.names[g].empty()) continue;
            Rooted<Value> key{rtMakeString(pieces.names[g])};
            Rooted<Value> value{pieces.present[g] ? stringOf(pieces.captures[g])
                                                  : Value::fromUndefined()};
            groups.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, value);
        }
    }

    // The argument vector is filled only once every allocation above is done.
    // A Value read out of its root any earlier is a raw pointer, and the next
    // allocation to trigger a collection moves what it points at — leaving a
    // replacer that is handed the right number of arguments and the wrong
    // bytes.
    std::vector<Value> argv;
    argv.reserve(roots.size() + 4);
    argv.push_back(matched.get());
    for (auto& r : roots) argv.push_back(r.get());
    argv.push_back(Value::fromDouble(static_cast<double>(pieces.start)));
    argv.push_back(whole.get());
    if (!pieces.names.empty()) argv.push_back(groups.get());
    Rooted<Value> undefinedThis;
    return Value(bronze_dynamic_call(fn.get().rawBits(), undefinedThis.get().rawBits(),
                                     static_cast<uint32_t>(argv.size()),
                                     reinterpret_cast<const uint64_t*>(argv.data())));
}

// One replacement, appended to `out`. Shared by every combination of
// (string pattern, RegExp pattern) x (string replacement, function
// replacement), which is why it takes the pieces rather than a match.
bool appendReplacement(Units& out, const Units& input, const MatchPieces& pieces,
                       Rooted<Value>& replacement, bool replacerIsFunction) {
    if (!replacerIsFunction) {
        const Units text = unitsOf(replacement.get());
        const Units expanded = substitute(text, input, pieces.start, pieces.end, pieces.captures,
                                          pieces.present, pieces.names);
        out.insert(out.end(), expanded.begin(), expanded.end());
        return true;
    }
    Value produced = callReplacer(replacement, pieces, input);
    if (rtExceptionPending()) return false;
    Rooted<Value> text{rtValueToString(produced)};
    const Units piece = unitsOf(text.get());
    out.insert(out.end(), piece.begin(), piece.end());
    return true;
}

template <bool All>
uint64_t stringReplacePattern(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const char* method = All ? "replaceAll" : "replace";
    Value selfVal;
    if (!requireString(Value(thisBits), method, selfVal)) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> self{selfVal};
    const Units input = unitsOf(self.get());

    const bool replacerIsFunction = isCallable(args[1]);
    Rooted<Value> replacement{replacerIsFunction ? args[1] : rtValueToString(args[1])};

    Units out;
    if (!rtIsRegExp(args[0])) {
        // A STRING pattern is matched literally — no compilation, no escaping
        // question. `replace` takes the first occurrence and `replaceAll`
        // every one, and both still expand `$&` and friends.
        const Units needle = unitsOf(rtValueToString(args[0]));
        size_t at = 0;
        for (;;) {
            size_t found = std::string::npos;
            for (size_t i = at; i + needle.size() <= input.size(); ++i) {
                if (std::equal(needle.begin(), needle.end(), input.begin() + static_cast<std::ptrdiff_t>(i))) {
                    found = i;
                    break;
                }
            }
            if (found == std::string::npos) break;
            const Units before = slice(input, at, found);
            out.insert(out.end(), before.begin(), before.end());
            MatchPieces pieces;
            pieces.start = found;
            pieces.end = found + needle.size();
            if (!appendReplacement(out, input, pieces, replacement, replacerIsFunction)) {
                return Value::fromUndefined().rawBits();
            }
            at = found + needle.size();
            if constexpr (All) {
                // An empty needle matches at every position; without this the
                // loop would never advance past the first one.
                if (needle.empty()) {
                    if (at < input.size()) out.push_back(input[at]);
                    ++at;
                }
            } else {
                break;
            }
        }
        const Units tail = slice(input, at, input.size());
        out.insert(out.end(), tail.begin(), tail.end());
        return stringOf(out).rawBits();
    }

    Rooted<Value> re{args[0]};
    const regex::Pattern& pattern = rtRegExpPattern(re.get());
    const regex::Flags& flags = regex::patternFlags(pattern);
    if constexpr (All) if (!flags.global) {
        // 22.1.3.20 step 2.c: `replaceAll` with a non-global RegExp is a
        // TypeError. `replace` with one replaces only the first, which is the
        // whole difference between them.
        return rtThrowTypeError(
                   "String.prototype.replaceAll called with a non-global RegExp argument")
            .rawBits();
    }
    const bool everyMatch = flags.global;
    const regex::Units haystack = toRegexUnits(input);
    if (everyMatch) rtRegExpSetLastIndex(re.get(), 0.0);

    size_t at = 0;
    size_t from = everyMatch ? 0 : static_cast<size_t>(rtRegExpLastIndex(re.get()));
    if (!everyMatch && !flags.sticky) from = 0;
    for (;;) {
        if (from > input.size()) break;
        regex::MatchResult match;
        if (runMatch(pattern, haystack, from, flags.sticky, match) != regex::ExecStatus::Match) {
            break;
        }
        const MatchPieces pieces = piecesOf(pattern, input, match);
        const Units before = slice(input, at, pieces.start);
        out.insert(out.end(), before.begin(), before.end());
        if (!appendReplacement(out, input, pieces, replacement, replacerIsFunction)) {
            return Value::fromUndefined().rawBits();
        }
        at = pieces.end;
        from = pieces.end == pieces.start ? pieces.end + 1 : pieces.end;
        if (!everyMatch) break;
    }
    if (everyMatch) rtRegExpSetLastIndex(re.get(), 0.0);
    const Units tail = slice(input, at, input.size());
    out.insert(out.end(), tail.begin(), tail.end());
    return stringOf(out).rawBits();
}

// ---- split ------------------------------------------------------------------

uint64_t stringSplitPattern(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!requireString(Value(thisBits), "split", selfVal)) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> self{selfVal};
    const Units input = unitsOf(self.get());
    Rooted<Value> re{args[0]};
    const regex::Pattern& pattern = rtRegExpPattern(re.get());
    const regex::Units haystack = toRegexUnits(input);
    const uint32_t groups = regex::captureCount(pattern);

    // 22.1.3.23 step 6: a limit of 0 answers an empty array whatever the
    // separator, before anything is matched.
    double limit = args.count() > 1 && !args[1].isUndefined() ? rtToNumber(args[1]) : 4294967295.0;
    if (std::isnan(limit) || limit < 0) limit = 0;

    // Length ZERO, grown by the appends below: `bronze_create_array(n)` sets
    // the length, so passing a capacity guess would leave trailing `undefined`
    // elements in the result.
    Rooted<Value> out{Value(bronze_create_array(0))};
    uint32_t count = 0;
    auto push = [&](Rooted<Value>& piece) {
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), count++, piece);
    };
    if (limit == 0) return out.get().rawBits();

    // 22.2.6.14 step 16: an EMPTY input either yields one empty string or, if
    // the pattern matches it, nothing at all.
    if (input.empty()) {
        regex::MatchResult match;
        if (runMatch(pattern, haystack, 0, false, match) == regex::ExecStatus::Match) {
            return out.get().rawBits();
        }
        Rooted<Value> whole{stringOf(input)};
        push(whole);
        return out.get().rawBits();
    }

    size_t sliceStart = 0;
    size_t at = 0;
    while (at < input.size()) {
        regex::MatchResult match;
        if (runMatch(pattern, haystack, at, true, match) != regex::ExecStatus::Match) {
            ++at;
            continue;
        }
        const auto end = static_cast<size_t>(match.end());
        // A separator that matched EMPTY at the position the last piece ended
        // would produce an infinite run of empty strings; 22.2.6.14 step 19.d
        // steps past it instead.
        if (end == sliceStart) {
            ++at;
            continue;
        }
        Rooted<Value> piece{stringOf(slice(input, sliceStart, at))};
        push(piece);
        if (static_cast<double>(count) == limit) return out.get().rawBits();
        for (uint32_t g = 1; g <= groups; ++g) {
            const int64_t from = match.captures[static_cast<size_t>(g) * 2];
            const int64_t to = match.captures[static_cast<size_t>(g) * 2 + 1];
            Rooted<Value> captured;
            if (from != regex::MatchResult::kUnset) {
                captured.set(stringOf(slice(input, static_cast<size_t>(from),
                                            static_cast<size_t>(to))));
            }
            push(captured);
            if (static_cast<double>(count) == limit) return out.get().rawBits();
        }
        sliceStart = end;
        at = end;
    }
    if (static_cast<double>(count) == limit) return out.get().rawBits();
    Rooted<Value> tail{stringOf(slice(input, sliceStart, input.size()))};
    push(tail);
    return out.get().rawBits();
}

struct PatternMethod {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const PatternMethod kPatternMethods[] = {
    {"match", stringMatch, 1},
    {"matchAll", stringMatchAll, 1},
    {"replace", stringReplacePattern<false>, 2},
    {"replaceAll", stringReplacePattern<true>, 2},
    {"search", stringSearch, 1},
};

}  // namespace

Value rtStringPatternMethod(const std::string& key) {
    for (const PatternMethod& m : kPatternMethods) {
        if (key == m.name) return Value(bronze_function_singleton(m.code, m.arity));
    }
    return Value::fromUndefined();
}

uint64_t rtStringSplitWithRegExp(uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return stringSplitPattern(0, thisBits, argc, argv);
}

}  // namespace bronze::runtime
