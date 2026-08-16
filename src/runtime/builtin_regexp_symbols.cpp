// `RegExp.prototype`'s SYMBOL-keyed members (ECMA-262 22.2.6): `[@@match]`,
// `[@@matchAll]`, `[@@replace]`, `[@@search]` and `[@@split]`.
//
// These are the real algorithms. `String.prototype.match`, `.matchAll`,
// `.replace`, `.replaceAll`, `.search` and `.split` are triage in front of them
// (builtin_string_regexp.cpp): each looks its argument up by the well-known
// key, and what it finds decides the whole algorithm. For a RegExp argument
// what it finds is one of these five, so the six string members and the five
// here are one implementation seen from two ends rather than two that must be
// kept in step.
//
// The five are reachable as FUNCTION OBJECTS too — `/x/[Symbol.replace]("ax",
// "y")` is a call a program may write — and reach them the way every other
// RegExp member does: beside the value, from rt_prop_symbol.cpp, because a
// RegExp carries no shape and bronze builds it no prototype object to hang a
// property on. `rtRegExpSymbolMethod` is the one table both routes read, so the
// function a program calls explicitly and the code the string members run are
// the same code pointer.
//
// Everything here drives the matcher DIRECTLY rather than through `exec`: a
// `replace` over a long string builds one result from many matches, and a match
// array per match would allocate an array per match. The consequence is stated
// as a refusal rather than left implicit — a subclass overriding `exec` cannot
// be honoured, and `rtCheckNativeBaseExtends` refuses `extends RegExp` by name
// (native_base.cpp), which is what keeps that from being reachable at all.

#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "regex/regex.h"
#include "runtime/array.h"
#include "runtime/builtin_string_regexp_internal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

using string_regexp::appendReplacement;
using string_regexp::isCallable;
using string_regexp::MatchPieces;
using string_regexp::slice;
using string_regexp::stringOf;
using string_regexp::Units;
using string_regexp::unitsOf;

namespace {

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
// not knowing the answer, which stays a hard error rather than becoming a
// catchable throw.
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

// 22.2.7.3 AdvanceStringIndex, as the cursor step every member here shares. An
// empty match, a separator that matched nothing, and the walk `split` takes
// over a position it could not match at are all the same operation — and under
// `u` all three step over a whole CHARACTER, so a cursor never lands between
// the halves of a surrogate pair.
size_t advanceOver(const regex::Units& haystack, size_t index, bool unicode) {
    return regex::advanceStringIndex(haystack, index, unicode);
}

// The same step applied to a RegExp's own `lastIndex`.
void advanceLastIndex(Value re, const regex::Units& haystack) {
    const bool unicode = regex::patternFlags(rtRegExpPattern(re)).unicodeMode();
    const auto index = static_cast<size_t>(rtRegExpLastIndex(re));
    rtRegExpSetLastIndex(re, static_cast<double>(advanceOver(haystack, index, unicode)));
}

// ---- the matchAll iterator (22.2.9) -----------------------------------------

// A plain object with `next` and the `[Symbol.iterator]` it inherits, exactly
// as a Map's iterators are — and its state is the INTERNAL SLOTS of 22.2.9.1:
// [[IteratingRegExp]], [[IteratedString]] and [[Done]]. Real fields, so nothing
// that enumerates an object can see them, `getOwnPropertyNames` included.
//
// [[Global]] is not a slot here because it need not be: the matcher this
// iterator holds is a clone nothing else can reach, so its flags cannot change
// under it and asking the compiled pattern is the same answer the slot would
// have carried.
Value readSlot(Rooted<Value>& obj, uint32_t slot) {
    return obj.get().asObject<ObjectHeader>()->internalSlot(slot);
}

void writeSlot(Rooted<Value>& obj, uint32_t slot, Value val) {
    obj.get().asObject<ObjectHeader>()->setInternalSlot(slot, val);
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

uint64_t matchAllNext(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    Rooted<Value> none;
    // 22.2.9.2.1 step 3: a receiver without the internal slots is a TypeError.
    // The brand is also what makes the slot reads below safe.
    if (!rtIsIteratorObject(self.get(), IteratorProto::RegExpString)) {
        return rtThrowTypeError("next called on an incompatible receiver").rawBits();
    }
    if (readSlot(self, RegExpStringIteratorSlot::Done).asBool()) {
        return iterResult(none, true).rawBits();
    }

    Rooted<Value> re{readSlot(self, RegExpStringIteratorSlot::IteratingRegExp)};
    Rooted<Value> input{readSlot(self, RegExpStringIteratorSlot::IteratedString)};
    Value result = rtRegExpExec(re, input);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    if (result.isNull()) {
        writeSlot(self, RegExpStringIteratorSlot::Done, Value::fromBool(true));
        return iterResult(none, true).rawBits();
    }
    Rooted<Value> match{result};
    // 22.2.9.2.1 step 8.a: a NON-GLOBAL matcher yields one match and is then
    // done. Without it the same match would be yielded for ever, because a
    // pattern with neither `g` nor `y` ignores `lastIndex` and re-matches from
    // zero every time. `String.prototype.matchAll` never builds one — it is a
    // TypeError there — so this is reachable only through an explicit
    // `/a/[Symbol.matchAll](s)`.
    if (!regex::patternFlags(rtRegExpPattern(re.get())).global) {
        writeSlot(self, RegExpStringIteratorSlot::Done, Value::fromBool(true));
        return iterResult(match, false).rawBits();
    }
    // 22.2.9.2.1 step 8.e.iii: an empty match advances the cursor by
    // AdvanceStringIndex, or the iterator would yield it for ever.
    Value first = match.get().asObject<ArrayHeader>()->getElem(0);
    if (first.isString() && first.asString<StringHeader>()->getLength() == 0) {
        advanceLastIndex(re.get(), toRegexUnits(unitsOf(input.get())));
    }
    return iterResult(match, false).rawBits();
}

}  // namespace

// ---- 22.2.6.12 [@@search] ---------------------------------------------------

Value rtRegExpSearch(Rooted<Value>& re, Rooted<Value>& str) {
    const Units input = unitsOf(str.get());
    // Steps 4-8: `search` SAVES and restores `lastIndex`, so it is the one
    // pattern member with no effect on the cursor.
    const double saved = rtRegExpLastIndex(re.get());
    regex::MatchResult match;
    const regex::Pattern& pattern = rtRegExpPattern(re.get());
    const regex::ExecStatus status = runMatch(pattern, toRegexUnits(input), 0, false, match);
    rtRegExpSetLastIndex(re.get(), saved);
    if (status != regex::ExecStatus::Match) return Value::fromDouble(-1.0);
    return Value::fromDouble(static_cast<double>(match.start()));
}

// ---- 22.2.6.8 [@@match] -----------------------------------------------------

Value rtRegExpMatch(Rooted<Value>& re, Rooted<Value>& str) {
    const regex::Flags& flags = regex::patternFlags(rtRegExpPattern(re.get()));
    // Step 5: without `g`, `[@@match]` IS `exec` — the same match array,
    // captures and all. With `g` it is the list of matched TEXTS and nothing
    // else, which is why the two answers have different shapes.
    if (!flags.global) return rtRegExpExec(re, str);

    rtRegExpSetLastIndex(re.get(), 0.0);
    const regex::Units haystack = toRegexUnits(unitsOf(str.get()));
    // Length ZERO, grown by the appends below: `bronze_create_array(n)` sets
    // the length, so passing a capacity guess would leave trailing `undefined`
    // elements in the result.
    Rooted<Value> out{Value(bronze_create_array(0))};
    uint32_t count = 0;
    for (;;) {
        Value result = rtRegExpExec(re, str);
        if (rtExceptionPending()) return Value::fromUndefined();
        if (result.isNull()) break;
        Rooted<Value> matched{result.asObject<ArrayHeader>()->getElem(0)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), count++, matched);
        // An empty match would leave `lastIndex` where it is and loop for
        // ever; step 8.f.iii advances it by AdvanceStringIndex.
        if (matched.get().asString<StringHeader>()->getLength() == 0) {
            advanceLastIndex(re.get(), haystack);
        }
    }
    // Step 6.c: `null`, not an empty array, when nothing matched.
    if (count == 0) return Value::fromNull();
    return out.get();
}

// ---- 22.2.6.9 [@@matchAll] --------------------------------------------------

Value rtRegExpMatchAll(Rooted<Value>& re, Rooted<Value>& str) {
    // Steps 4-6: the iterator matches with a CLONE, so its cursor is its own
    // and a `for-of` cannot move the caller's `lastIndex`. Cloned through
    // `source` and `flags` because that is the pair a RegExp is made of, and
    // because 22.2.6.10's escaping is idempotent — the source of the clone is
    // the source of the original, byte for byte.
    Rooted<Value> source{rtRegExpMember(re.get(), "source")};
    const std::string flagsText =
        rtUtf8Chars(rtRegExpMember(re.get(), "flags").asString<StringHeader>());
    Rooted<Value> matcher{rtRegExpFromParts(source, flagsText)};
    if (rtExceptionPending()) return Value::fromUndefined();

    // %RegExpStringIteratorPrototype% (22.2.9.1), which is where the
    // `[Symbol.iterator]` self-hook lives — inherited from %IteratorPrototype%,
    // so this object has no own symbol-keyed property.
    Rooted<Value> it{rtNewIteratorObject(IteratorProto::RegExpString)};
    Rooted<Value> nextFn{rtNativeFunction(matchAllNext, 0)};
    Rooted<Value> nk{rtMakeString("next")};
    it.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), nk, nextFn);
    writeSlot(it, RegExpStringIteratorSlot::IteratingRegExp, matcher.get());
    writeSlot(it, RegExpStringIteratorSlot::IteratedString, str.get());
    writeSlot(it, RegExpStringIteratorSlot::Done, Value::fromBool(false));
    return it.get();
}

// ---- 22.2.6.11 [@@replace] --------------------------------------------------

Value rtRegExpReplace(Rooted<Value>& re, Rooted<Value>& str, Rooted<Value>& replaceValue) {
    const Units input = unitsOf(str.get());
    // Steps 5-6: a callable replaceValue is called per match; anything else is
    // ToString'd once and expanded by GetSubstitution.
    const bool replacerIsFunction = isCallable(replaceValue.get());
    Rooted<Value> replacement{replacerIsFunction ? replaceValue.get()
                                                 : rtValueToString(replaceValue.get())};
    if (rtExceptionPending()) return Value::fromUndefined();

    const regex::Pattern& pattern = rtRegExpPattern(re.get());
    const regex::Flags& flags = regex::patternFlags(pattern);
    // Step 4: `global` decides between "every match" and "the first", and it is
    // the whole difference between `"aa".replace(/a/g, ...)` and
    // `"aa".replace(/a/, ...)`.
    const bool everyMatch = flags.global;
    const regex::Units haystack = toRegexUnits(input);
    if (everyMatch) rtRegExpSetLastIndex(re.get(), 0.0);

    Units out;
    size_t at = 0;
    size_t from = everyMatch ? 0 : static_cast<size_t>(rtRegExpLastIndex(re.get()));
    // A non-global, non-sticky pattern ignores `lastIndex` entirely (22.2.7.2
    // step 6), so it always starts at zero; a `y` one starts at the cursor.
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
            return Value::fromUndefined();
        }
        at = pieces.end;
        // Step 9.d: an empty match steps by AdvanceStringIndex, which is what
        // keeps the loop moving and, under `u`, keeps it landing on character
        // boundaries.
        from = pieces.end == pieces.start ? advanceOver(haystack, pieces.end, flags.unicodeMode())
                                          : pieces.end;
        if (!everyMatch) break;
    }
    if (everyMatch) rtRegExpSetLastIndex(re.get(), 0.0);
    const Units tail = slice(input, at, input.size());
    out.insert(out.end(), tail.begin(), tail.end());
    return stringOf(out);
}

// ---- 22.2.6.14 [@@split] ----------------------------------------------------

Value rtRegExpSplit(Rooted<Value>& re, Rooted<Value>& str, Value limitArg) {
    const Units input = unitsOf(str.get());
    const regex::Pattern& pattern = rtRegExpPattern(re.get());
    const regex::Units haystack = toRegexUnits(input);
    const uint32_t groups = regex::captureCount(pattern);

    // Step 11: a limit of 0 answers an empty array whatever the separator,
    // before anything is matched.
    double limit = limitArg.isUndefined() ? 4294967295.0 : rtToNumber(limitArg);
    if (rtExceptionPending()) return Value::fromUndefined();
    if (std::isnan(limit) || limit < 0) limit = 0;

    // Length ZERO, grown by the appends below: `bronze_create_array(n)` sets
    // the length, so passing a capacity guess would leave trailing `undefined`
    // elements in the result.
    Rooted<Value> out{Value(bronze_create_array(0))};
    uint32_t count = 0;
    auto push = [&](Rooted<Value>& piece) {
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), count++, piece);
    };
    if (limit == 0) return out.get();

    // Step 16: an EMPTY input either yields one empty string or, if the pattern
    // matches it, nothing at all.
    if (input.empty()) {
        regex::MatchResult match;
        if (runMatch(pattern, haystack, 0, false, match) == regex::ExecStatus::Match) {
            return out.get();
        }
        Rooted<Value> whole{stringOf(input)};
        push(whole);
        return out.get();
    }

    // `q` walks the string by AdvanceStringIndex (steps 19.a and 19.d.i), so
    // under `u` the separator is never tried between the halves of a surrogate
    // pair and a piece never ends inside one.
    const bool unicode = regex::patternFlags(pattern).unicodeMode();
    size_t sliceStart = 0;
    size_t at = 0;
    while (at < input.size()) {
        regex::MatchResult match;
        if (runMatch(pattern, haystack, at, true, match) != regex::ExecStatus::Match) {
            at = advanceOver(haystack, at, unicode);
            continue;
        }
        const auto end = static_cast<size_t>(match.end());
        // A separator that matched EMPTY at the position the last piece ended
        // would produce an infinite run of empty strings; step 19.d steps past
        // it instead.
        if (end == sliceStart) {
            at = advanceOver(haystack, at, unicode);
            continue;
        }
        Rooted<Value> piece{stringOf(slice(input, sliceStart, at))};
        push(piece);
        if (static_cast<double>(count) == limit) return out.get();
        for (uint32_t g = 1; g <= groups; ++g) {
            const int64_t from = match.captures[static_cast<size_t>(g) * 2];
            const int64_t to = match.captures[static_cast<size_t>(g) * 2 + 1];
            Rooted<Value> captured;
            if (from != regex::MatchResult::kUnset) {
                captured.set(stringOf(slice(input, static_cast<size_t>(from),
                                            static_cast<size_t>(to))));
            }
            push(captured);
            if (static_cast<double>(count) == limit) return out.get();
        }
        sliceStart = end;
        at = end;
    }
    if (static_cast<double>(count) == limit) return out.get();
    Rooted<Value> tail{stringOf(slice(input, sliceStart, input.size()))};
    push(tail);
    return out.get();
}

// ---- the five as function objects -------------------------------------------

namespace {

// 22.2.6's opening step in every one of the five: `this` must be an Object with
// a [[RegExpMatcher]], which for bronze is a RegExp heap object and nothing
// else. `/x/[Symbol.replace].call("ax", "y")` is the TypeError this produces.
bool thisRegExp(Value self, const char* member) {
    if (rtIsRegExp(self)) return true;
    rtThrowTypeError(std::string("RegExp.prototype[Symbol.") + member +
                     "] called on a receiver that is not a RegExp");
    return false;
}

uint64_t regexpSymbolMatch(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!thisRegExp(Value(thisBits), "match")) return Value::fromUndefined().rawBits();
    Rooted<Value> re{Value(thisBits)};
    Rooted<Value> str{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtRegExpMatch(re, str).rawBits();
}

uint64_t regexpSymbolMatchAll(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!thisRegExp(Value(thisBits), "matchAll")) return Value::fromUndefined().rawBits();
    Rooted<Value> re{Value(thisBits)};
    Rooted<Value> str{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtRegExpMatchAll(re, str).rawBits();
}

uint64_t regexpSymbolReplace(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!thisRegExp(Value(thisBits), "replace")) return Value::fromUndefined().rawBits();
    Rooted<Value> re{Value(thisBits)};
    Rooted<Value> str{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Rooted<Value> replaceValue{args[1]};
    return rtRegExpReplace(re, str, replaceValue).rawBits();
}

uint64_t regexpSymbolSearch(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!thisRegExp(Value(thisBits), "search")) return Value::fromUndefined().rawBits();
    Rooted<Value> re{Value(thisBits)};
    Rooted<Value> str{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtRegExpSearch(re, str).rawBits();
}

uint64_t regexpSymbolSplit(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!thisRegExp(Value(thisBits), "split")) return Value::fromUndefined().rawBits();
    Rooted<Value> re{Value(thisBits)};
    Rooted<Value> str{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtRegExpSplit(re, str, args[1]).rawBits();
}

// The one table both routes read: the property path that answers
// `/x/[Symbol.replace]`, and nothing else — the string members call the
// algorithms above directly, so there is no second spelling of "which symbol
// means which algorithm" to drift out of step with this one.
//
// The key is compared by IDENTITY, which is the whole contract of a well-known
// symbol: a second interning of the description "Symbol.replace" would be a
// different symbol and would find nothing here.
struct RegExpSymbolMethod {
    SymbolHeader* (*key)();
    bronze_fn_code code;
    uint32_t arity;
};

const RegExpSymbolMethod kRegExpSymbolMethods[] = {
    {rtSymbolMatch, regexpSymbolMatch, 1},
    {rtSymbolMatchAll, regexpSymbolMatchAll, 1},
    {rtSymbolReplace, regexpSymbolReplace, 2},
    {rtSymbolSearch, regexpSymbolSearch, 1},
    {rtSymbolSplit, regexpSymbolSplit, 2},
};

}  // namespace

Value rtRegExpSymbolMethod(Value symbolKey) {
    if (!symbolKey.isSymbol()) return Value::fromUndefined();
    SymbolHeader* wanted = symbolKey.asSymbol<SymbolHeader>();
    for (const RegExpSymbolMethod& m : kRegExpSymbolMethods) {
        // Each `key()` interns its symbol on first use, which is why the
        // comparison is inside the loop rather than against a table built once:
        // the table holds the accessors, not their answers.
        if (m.key() == wanted) return rtNativeFunction(m.code, m.arity);
    }
    return Value::fromUndefined();
}

}  // namespace bronze::runtime
