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
// "y")` is a call a program may write — as own data properties of
// `RegExp.prototype` (`rtInstallRegExpSymbolMethods`), so the function a
// program calls explicitly and the code the string members run are the same
// code pointer, and `RegExp.prototype[Symbol.replace] = f` is an ordinary
// property write that `rtRegExpChainPristine` notices.
//
// Everything here drives the matcher DIRECTLY rather than through `exec`: a
// `replace` over a long string builds one result from many matches, and a match
// array per match would allocate an array per match. The consequence is stated
// as a refusal rather than left implicit — a receiver whose `exec` is not the
// intrinsic one (22.2.7.1 step 2 would call it) is refused BY NAME in each of
// the five, because answering from the matcher would be a silently wrong
// answer rather than a missing one.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "regex/regex.h"
#include "runtime/array.h"
#include "runtime/builtin_regexp_internal.h"
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

// The matcher, run with the exception machinery: a failure here throws a
// catchable RangeError.
regex::ExecStatus runMatch(const regex::Pattern& pattern, const regex::Units& input, size_t from,
                           bool sticky, regex::MatchResult& match) {
    std::string error;
    const regex::ExecStatus status =
        sticky ? regex::matchAt(pattern, input, from, match, error)
               : regex::search(pattern, input, from, match, error);
    if (status == regex::ExecStatus::Error) {
        rtThrowRangeError(error.empty() ? "regular expression execution limit exceeded"
                                        : error.c_str());
        return regex::ExecStatus::Error;
    }
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

// The same step applied to a RegExp's own `lastIndex`: ToLength of what was
// assigned (which may run user code), then Set(R, "lastIndex", ..., true),
// which a frozen RegExp refuses. False with the exception pending.
bool advanceLastIndex(Rooted<Value>& re, const regex::Units& haystack) {
    const bool unicode = regex::patternFlags(rtRegExpPattern(re.get())).unicodeMode();
    bool ok = false;
    const auto index = static_cast<size_t>(rtRegExpLastIndexLength(re, ok));
    if (!ok) return false;
    return rtRegExpSetLastIndex(re, static_cast<double>(advanceOver(haystack, index, unicode)));
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

Value iterResult(Rooted<Value>& value, bool done) { return rtCreateIterResult(value, done); }

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
        if (!advanceLastIndex(re, toRegexUnits(unitsOf(input.get())))) {
            return Value::fromUndefined().rawBits();
        }
    }
    return iterResult(match, false).rawBits();
}

}  // namespace

// ---- 22.2.6.12 [@@search] ---------------------------------------------------

Value rtRegExpSearch(Rooted<Value>& re, Rooted<Value>& str) {
    const Units input = unitsOf(str.get());
    // Steps 4-8: `search` SAVES and restores `lastIndex`, so it is the one
    // pattern member with no effect on the cursor. Restored as the VALUE it
    // held, not a number made from it. Step 5 is a real Set when the cursor is
    // not already +0 — the one way a frozen RegExp's `search` can throw.
    Rooted<Value> saved{rtRegExpLastIndexValue(re.get())};
    const bool atZero = saved.get().isNumber() && saved.get().asNumber() == 0.0 &&
                        !std::signbit(saved.get().asNumber());
    if (!atZero && !rtRegExpSetLastIndex(re, 0.0)) return Value::fromUndefined();
    regex::MatchResult match;
    const regex::Pattern& pattern = rtRegExpPattern(re.get());
    const regex::ExecStatus status = runMatch(pattern, toRegexUnits(input), 0, false, match);
    rtRegExpRestoreLastIndex(re.get(), saved.get());
    if (status == regex::ExecStatus::Error || rtExceptionPending()) return Value::fromUndefined();
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

    if (!rtRegExpSetLastIndex(re, 0.0)) return Value::fromUndefined();
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
            if (!advanceLastIndex(re, haystack)) return Value::fromUndefined();
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
    Rooted<Value> source{re.get().asObject<RegExpHeader>()->source};
    const std::string flagsText =
        rtUtf8Chars(re.get().asObject<RegExpHeader>()->flagsText.asString<StringHeader>());
    Rooted<Value> matcher{rtRegExpFromParts(source, flagsText)};
    if (rtExceptionPending()) return Value::fromUndefined();
    // Step 6: the clone STARTS WHERE THE ORIGINAL STOOD. `rtRegExpFromParts`
    // builds a fresh pattern with `lastIndex` at zero, which is a wrong answer
    // rather than a missing one — `re.lastIndex = 2; [..."aaaa".matchAll(re)]`
    // has two matches, not four. Copied through ToLength (7.1.20), so a
    // negative cursor is 0 and a fractional one truncates, exactly as a
    // subsequent `exec` on the original would have read it.
    bool ok = false;
    const double cursor = rtRegExpLastIndexLength(re, ok);
    if (!ok) return Value::fromUndefined();
    rtRegExpSetLastIndex(matcher, cursor);

    // %RegExpStringIteratorPrototype% (22.2.9.1), which is where the
    // `[Symbol.iterator]` self-hook lives — inherited from %IteratorPrototype%,
    // so this object has no own symbol-keyed property.
    Rooted<Value> it{rtNewIteratorObject(IteratorProto::RegExpString)};
    Rooted<Value> nextFn{rtNativeFunction(matchAllNext, 0, "next", 0)};
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
    if (everyMatch && !rtRegExpSetLastIndex(re, 0.0)) return Value::fromUndefined();

    Units out;
    size_t at = 0;
    size_t from = 0;
    // A non-global, non-sticky pattern ignores `lastIndex` entirely (22.2.7.2
    // step 6), so it always starts at zero; a `y` one starts at the cursor.
    if (!everyMatch && flags.sticky) {
        bool ok = false;
        from = static_cast<size_t>(rtRegExpLastIndexLength(re, ok));
        if (!ok) return Value::fromUndefined();
    }
    // A `y` pattern's one exec (22.2.7.2 steps 12-15) leaves `lastIndex` at
    // the match's end, or at 0 when it failed — before the replacer runs,
    // because that is where step 11 of [@@replace] puts it: the exec is
    // finished before any replacement is computed.
    const bool stickyOnly = !everyMatch && flags.sticky;
    for (;;) {
        regex::MatchResult match;
        if (from > input.size()) {
            if (stickyOnly && !rtRegExpSetLastIndex(re, 0.0)) return Value::fromUndefined();
            break;
        }
        const regex::ExecStatus status = runMatch(pattern, haystack, from, flags.sticky, match);
        if (status == regex::ExecStatus::Error || rtExceptionPending()) {
            return Value::fromUndefined();
        }
        if (status != regex::ExecStatus::Match) {
            if (stickyOnly && !rtRegExpSetLastIndex(re, 0.0)) return Value::fromUndefined();
            break;
        }
        const MatchPieces pieces = piecesOf(pattern, input, match);
        if (stickyOnly && !rtRegExpSetLastIndex(re, static_cast<double>(pieces.end))) {
            return Value::fromUndefined();
        }
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
    // The global walk ends on an exec that failed, which is what put the
    // cursor back at 0 (22.2.7.2 step 12.a.i).
    if (everyMatch && !rtRegExpSetLastIndex(re, 0.0)) return Value::fromUndefined();
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
        const regex::ExecStatus status = runMatch(pattern, haystack, 0, false, match);
        if (status == regex::ExecStatus::Error || rtExceptionPending()) {
            return Value::fromUndefined();
        }
        if (status == regex::ExecStatus::Match) {
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
        const regex::ExecStatus status = runMatch(pattern, haystack, at, true, match);
        if (status == regex::ExecStatus::Error || rtExceptionPending()) {
            return Value::fromUndefined();
        }
        if (status != regex::ExecStatus::Match) {
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
//
// Then the refusal the file comment states. A pristine receiver's `exec` is
// the intrinsic by construction (`rtRegExpChainPristine` compares that slot);
// any other receiver — a subclass instance, a RegExp with an own key — has
// its `exec` READ, the Get of 22.2.7.1 step 1, and is refused by name unless
// what it holds runs the intrinsic body. A subclass that leaves `exec` alone
// therefore works; one that overrides it is told so rather than answered from
// a matcher its `exec` never saw.
bool receiverFor(Rooted<Value>& self, const char* member) {
    if (!rtIsRegExp(self.get())) {
        rtThrowTypeError(std::string("RegExp.prototype[Symbol.") + member +
                         "] called on a receiver that is not a RegExp");
        return false;
    }
    if (rtRegExpChainPristine(self.get().asObject<RegExpHeader>())) return true;
    Rooted<Value> key{rtMakeString("exec")};
    const Value exec{bronze_elem_get(self.get().rawBits(), key.get().rawBits())};
    if (rtExceptionPending()) return false;
    if (exec.isObject() && exec.asObject<HeapObjectHeader>()->flags == HeapKind::Function &&
        exec.asObject<FunctionHeader>()->code == rtRegExpExecBody) {
        return true;
    }
    fatal((std::string("RegExp.prototype[Symbol.") + member +
           "] on a RegExp whose `exec` is not RegExp.prototype.exec: bronze drives the matcher "
           "directly and does not call an overridden exec")
              .c_str());
    return false;
}

uint64_t regexpSymbolMatch(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> re{Value(thisBits)};
    if (!receiverFor(re, "match")) return Value::fromUndefined().rawBits();
    Rooted<Value> str{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtRegExpMatch(re, str).rawBits();
}

uint64_t regexpSymbolMatchAll(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> re{Value(thisBits)};
    if (!receiverFor(re, "matchAll")) return Value::fromUndefined().rawBits();
    Rooted<Value> str{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtRegExpMatchAll(re, str).rawBits();
}

uint64_t regexpSymbolReplace(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> re{Value(thisBits)};
    if (!receiverFor(re, "replace")) return Value::fromUndefined().rawBits();
    Rooted<Value> str{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Rooted<Value> replaceValue{args[1]};
    return rtRegExpReplace(re, str, replaceValue).rawBits();
}

uint64_t regexpSymbolSearch(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> re{Value(thisBits)};
    if (!receiverFor(re, "search")) return Value::fromUndefined().rawBits();
    Rooted<Value> str{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtRegExpSearch(re, str).rawBits();
}

uint64_t regexpSymbolSplit(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> re{Value(thisBits)};
    if (!receiverFor(re, "split")) return Value::fromUndefined().rawBits();
    Rooted<Value> str{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtRegExpSplit(re, str, args[1]).rawBits();
}

// The five as `RegExp.prototype`'s own data properties, in 22.2.6's clause
// order, which is also the order node lists them. The string members call
// the algorithms above directly, so there is no second spelling of "which
// symbol means which algorithm" to drift out of step with this one.
struct RegExpSymbolMethod {
    SymbolHeader* (*key)();
    bronze_fn_code code;
    uint32_t arity;
    // 10.2.9 step 4: a symbol-keyed function is named "[description]".
    const char* name;
    uint32_t length;
};

const RegExpSymbolMethod kRegExpSymbolMethods[] = {
    {rtSymbolMatch, regexpSymbolMatch, 1, "[Symbol.match]", 1},
    {rtSymbolMatchAll, regexpSymbolMatchAll, 1, "[Symbol.matchAll]", 1},
    {rtSymbolReplace, regexpSymbolReplace, 2, "[Symbol.replace]", 2},
    {rtSymbolSearch, regexpSymbolSearch, 1, "[Symbol.search]", 1},
    {rtSymbolSplit, regexpSymbolSplit, 2, "[Symbol.split]", 2},
};

}  // namespace

void rtInstallRegExpSymbolMethods(Rooted<Value>& proto) {
    for (const RegExpSymbolMethod& m : kRegExpSymbolMethods) {
        Rooted<Value> key{Value::fromSymbol(m.key())};
        Rooted<Value> fn{rtNativeFunction(m.code, m.arity, m.name, m.length)};
        // 22.2.6: writable and configurable, not enumerable — a definition,
        // so it does not write through to Object.prototype.
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, fn, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
}

}  // namespace bronze::runtime
