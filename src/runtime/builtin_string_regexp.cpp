// `String.prototype`'s members that take a PATTERN: `match`, `matchAll`,
// `replace`, `replaceAll`, `search` and `split` (ECMA-262 22.1.3), and the
// literal-string algorithms only they use.
//
// Every one of the six is TRIAGE and not an algorithm. 22.1.3 defines each as:
// look the argument up by a well-known symbol, and if it has a method there,
// hand the whole operation over to it. `"s".replace(x, r)` reads
// `x[Symbol.replace]`; a RegExp answers with 22.2.6.11 and so the familiar
// behaviour is a DISPATCH like any other, and an object that defines its own
// `[Symbol.replace]` is a first-class pattern with nothing special about it.
// The five algorithms live in builtin_regexp_symbols.cpp; what is here is which
// one a given argument names.
//
// The dispatch costs the common path nothing. `rtPatternMethod` answers "no
// method" from the argument's TAG for a string, a number or `undefined`, and
// from its heap kind for a RegExp — bronze builds no `RegExp.prototype` object
// and a RegExp carries no shape, so a RegExp's five symbol-keyed members are
// answered beside the value (rt_prop_symbol.cpp) and there is no own key that
// could shadow them. `"str".replace(/re/, "x")` therefore reaches
// `rtRegExpReplace` through one tag test and one flags compare, exactly as it
// did before the protocol existed, and only an ordinary OBJECT argument pays
// for a symbol-keyed property read.
//
// Separate from builtin_string.cpp because these are the only string members
// that know what a RegExp is, and because their argument is not a string: a
// string argument to `replace` or `split` is matched LITERALLY (`"a.c"
// .split(".")` splits on a dot, `"a.c".split(/./)` splits on everything), which
// is the algorithm this file owns.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "regex/regex.h"
#include "runtime/builtin_string_regexp_internal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
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

// The receiver as a string, or a TypeError. Every member here is defined on
// `String.prototype`, so a `this` that is neither a string nor a String object
// is 22.1.3's RequireObjectCoercible failure and not something to compute over.
bool requireString(Value self, const char* method, Value& out) {
    if (rtThisStringValue(self, out)) return true;
    rtThrowTypeError(std::string("String.prototype.") + method +
                     " called on a value that is not a string");
    return false;
}

// The RegExp a pattern argument denotes, made if the argument was not one.
// `extraFlags` is what `matchAll` needs: 22.1.3.14 requires a `g` pattern, and
// a bare string argument becomes one.
Value patternArgument(Value arg, const char* extraFlags) {
    if (rtIsRegExp(arg)) return arg;
    Rooted<Value> source{arg.isUndefined() ? rtMakeString("") : rtValueToString(arg)};
    return rtRegExpFromParts(source, extraFlags);
}

// The well-known key a member dispatches on, and the text of it for a message.
// One switch rather than five, so a member and its key are one line.
SymbolHeader* patternSymbolKey(PatternSymbol which) {
    switch (which) {
        case PatternSymbol::Match: return rtSymbolMatch();
        case PatternSymbol::MatchAll: return rtSymbolMatchAll();
        case PatternSymbol::Replace: return rtSymbolReplace();
        case PatternSymbol::Search: return rtSymbolSearch();
        case PatternSymbol::Split: return rtSymbolSplit();
    }
    fatal("internal: a pattern dispatch on an unknown well-known symbol");
}

const char* patternSymbolName(PatternSymbol which) {
    switch (which) {
        case PatternSymbol::Match: return "match";
        case PatternSymbol::MatchAll: return "matchAll";
        case PatternSymbol::Replace: return "replace";
        case PatternSymbol::Search: return "search";
        case PatternSymbol::Split: return "split";
    }
    fatal("internal: a pattern dispatch on an unknown well-known symbol");
}

// 7.2.8 IsRegExp, as `matchAll` and `replaceAll` ask it (22.1.3.14 step 2.a,
// 22.1.3.20 step 2.a): an object whose `[Symbol.match]` is truthy is a regular
// expression for the purpose of the `g` requirement below, whether or not it
// carries a matcher.
//
// A real RegExp is answered from its heap kind with no property read: its
// `[Symbol.match]` is `RegExp.prototype[@@match]`, which is a function and so
// truthy, and nothing can have replaced it.
bool argumentIsRegExpLike(Rooted<Value>& arg, bool& threw) {
    threw = false;
    if (!arg.get().isObject()) return false;
    if (rtIsRegExp(arg.get())) return true;
    Rooted<Value> key{Value::fromSymbol(rtSymbolMatch())};
    const Value matcher = Value(bronze_elem_get(arg.get().rawBits(), key.get().rawBits()));
    if (rtExceptionPending()) {
        threw = true;
        return false;
    }
    // Step 2: `undefined` falls through to the [[RegExpMatcher]] question,
    // which for a non-RegExp object is `false`. Anything else is ToBoolean —
    // so an explicit `[Symbol.match] = false` opts an object OUT.
    if (matcher.isUndefined()) return false;
    return bronze_truthy(matcher.rawBits());
}

// 22.1.3.14 step 2.b and 22.1.3.20 step 2.b: `matchAll` and `replaceAll` given
// a regexp-like argument require its flags to contain `g`, and the check runs
// BEFORE the dispatch — a non-global pattern is a TypeError even when the
// argument's own `[Symbol.matchAll]` would have ignored the flags entirely.
// "All of them" is not something a non-global pattern can deliver, so the
// method refuses rather than silently delivering one.
bool requireGlobalPattern(Rooted<Value>& arg, const char* method) {
    bool threw = false;
    if (!argumentIsRegExpLike(arg, threw)) return !threw;
    const std::string message =
        std::string("String.prototype.") + method + " called with a non-global RegExp argument";
    if (rtIsRegExp(arg.get())) {
        if (regex::patternFlags(rtRegExpPattern(arg.get())).global) return true;
        rtThrowTypeError(message);
        return false;
    }
    Rooted<Value> key{rtMakeString("flags")};
    const Value flags = Value(bronze_elem_get(arg.get().rawBits(), key.get().rawBits()));
    if (rtExceptionPending()) return false;
    // Step 2.b.ii RequireObjectCoercible: `flags` being absent is its own
    // failure and not an absent `g`, so it says so.
    if (flags.isUndefined() || flags.isNull()) {
        rtThrowTypeError(std::string("String.prototype.") + method +
                         " called with a RegExp-like argument whose `flags` is " +
                         (flags.isNull() ? "null" : "undefined"));
        return false;
    }
    Rooted<Value> text{rtValueToString(flags)};
    if (rtExceptionPending()) return false;
    if (rtUtf8Chars(text.get().asString<StringHeader>()).find('g') != std::string::npos) {
        return true;
    }
    rtThrowTypeError(message);
    return false;
}

// ---- search -----------------------------------------------------------------

uint64_t stringSearch(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!requireString(Value(thisBits), "search", selfVal)) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> self{selfVal};
    Rooted<Value> arg{args[0]};
    Rooted<Value> searcher;
    // 22.1.3.22 step 2.
    if (rtPatternMethod(arg, PatternSymbol::Search, searcher)) {
        return rtCallPatternMethod(searcher, arg, self, self, /*argCount=*/1).rawBits();
    }
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    if (!rtIsRegExp(args[0])) {
        // Step 3 creates a RegExp from the argument, so a string argument is
        // PATTERN TEXT here — unlike `replace` and `split`, where a string is
        // matched literally. `"a.c".search(".")` is 0.
        //
        // An ABSENT argument is the empty pattern, not the text "undefined":
        // 22.2.3.1 RegExpInitialize step 1 makes P the empty String when
        // `pattern` is undefined, and the empty pattern matches at 0 — so
        // `"x".search()` is 0 where ToString(undefined) would compile
        // /undefined/ and answer -1.
        Rooted<Value> source{args[0].isUndefined() ? rtMakeString("")
                                                   : rtValueToString(args[0])};
        Rooted<Value> made{rtRegExpFromParts(source, "")};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        return rtRegExpSearch(made, self).rawBits();
    }
    Rooted<Value> re{args[0]};
    return rtRegExpSearch(re, self).rawBits();
}

// ---- match / matchAll -------------------------------------------------------

uint64_t stringMatch(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!requireString(Value(thisBits), "match", selfVal)) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> self{selfVal};
    Rooted<Value> arg{args[0]};
    Rooted<Value> matcher;
    // 22.1.3.13 step 2.
    if (rtPatternMethod(arg, PatternSymbol::Match, matcher)) {
        return rtCallPatternMethod(matcher, arg, self, self, /*argCount=*/1).rawBits();
    }
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    Rooted<Value> re{patternArgument(args[0], "")};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtRegExpMatch(re, self).rawBits();
}

uint64_t stringMatchAll(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!requireString(Value(thisBits), "matchAll", selfVal)) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> self{selfVal};
    Rooted<Value> arg{args[0]};
    // Step 2.a-b, ahead of the dispatch.
    if (!requireGlobalPattern(arg, "matchAll")) return Value::fromUndefined().rawBits();
    Rooted<Value> matcher;
    // Step 2.c.
    if (rtPatternMethod(arg, PatternSymbol::MatchAll, matcher)) {
        return rtCallPatternMethod(matcher, arg, self, self, /*argCount=*/1).rawBits();
    }
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    // Steps 4-5: a non-RegExp argument becomes a `g` pattern, and the operation
    // is then 22.2.6.9 on it — which clones, so the iterator's cursor is its
    // own either way and a `for-of` never moves the caller's `lastIndex`.
    if (rtIsRegExp(args[0])) {
        Rooted<Value> re{args[0]};
        return rtRegExpMatchAll(re, self).rawBits();
    }
    Rooted<Value> source{args[0].isUndefined() ? rtMakeString("") : rtValueToString(args[0])};
    Rooted<Value> made{rtRegExpFromParts(source, "g")};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtRegExpMatchAll(made, self).rawBits();
}

// ---- replace / replaceAll ---------------------------------------------------

// 22.1.3.19 step 3 / 22.1.3.20 step 8: a STRING search value is matched
// LITERALLY — no compilation, no escaping question. `replace` takes the first
// occurrence and `replaceAll` every one, and both still expand `$&` and
// friends, which is why this shares `appendReplacement` with 22.2.6.11.
template <bool All>
uint64_t replaceLiteral(Rooted<Value>& self, Rooted<Value>& searchValue,
                        Rooted<Value>& replaceValue) {
    const Units input = unitsOf(self.get());
    const bool replacerIsFunction = isCallable(replaceValue.get());
    Rooted<Value> replacement{replacerIsFunction ? replaceValue.get()
                                                 : rtValueToString(replaceValue.get())};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    const Units needle = unitsOf(rtValueToString(searchValue.get()));
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    Units out;
    size_t at = 0;
    for (;;) {
        size_t found = std::string::npos;
        for (size_t i = at; i + needle.size() <= input.size(); ++i) {
            if (std::equal(needle.begin(), needle.end(),
                           input.begin() + static_cast<std::ptrdiff_t>(i))) {
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
            // An empty needle matches at every position; without this the loop
            // would never advance past the first one.
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

template <bool All>
uint64_t stringReplacePattern(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const char* method = All ? "replaceAll" : "replace";
    Value selfVal;
    if (!requireString(Value(thisBits), method, selfVal)) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> self{selfVal};
    Rooted<Value> arg{args[0]};
    // 22.1.3.20 step 2.a-b, ahead of the dispatch. `replace` has no such step:
    // replacing the FIRST match is something any pattern can do.
    if constexpr (All) {
        if (!requireGlobalPattern(arg, method)) return Value::fromUndefined().rawBits();
    }
    Rooted<Value> replaceValue{args[1]};
    Rooted<Value> replacer;
    // 22.1.3.19 step 2 / 22.1.3.20 step 2.c, and the argument order is the
    // whole reason `[Symbol.replace]` takes two: (string, replaceValue).
    if (rtPatternMethod(arg, PatternSymbol::Replace, replacer)) {
        return rtCallPatternMethod(replacer, arg, self, replaceValue, /*argCount=*/2).rawBits();
    }
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    if (rtIsRegExp(args[0])) {
        Rooted<Value> re{args[0]};
        return rtRegExpReplace(re, self, replaceValue).rawBits();
    }
    return replaceLiteral<All>(self, arg, replaceValue);
}

const NativeMethod kPatternMethods[] = {
    {"match", stringMatch, 1},
    {"matchAll", stringMatchAll, 1},
    {"replace", stringReplacePattern<false>, 2},
    {"replaceAll", stringReplacePattern<true>, 2},
    {"search", stringSearch, 1},
};

}  // namespace

void rtInstallStringPatternMethods(Rooted<Value>& proto) {
    rtDefineMethods(proto, kPatternMethods, std::size(kPatternMethods));
}

bool rtPatternMethod(Rooted<Value>& arg, PatternSymbol which, Rooted<Value>& out) {
    // THE GUARD. Both tests are the argument's own bits: a string, a number,
    // `undefined` and `null` are not objects and reach `String.prototype` or
    // nothing, neither of which defines one of the five; a RegExp answers its
    // five beside the value and has no shape for an own key to shadow them
    // with. So the two shapes of argument that every real program passes cost
    // one tag test and never touch the property path.
    //
    // The day a RegExp gains a shape — a subclass instance would be the reason,
    // and `extends RegExp` is refused by name today (native_base.cpp) — this
    // second test is the line that has to become "and no own key", and the
    // dispatch below is already what would honour the override.
    if (!arg.get().isObject()) return false;
    if (arg.get().asObject<HeapObjectHeader>()->flags == RegExpHeader::kFlags) return false;

    // GetMethod (7.3.11) from here down: the key is interned before the
    // receiver is re-read, because interning one allocates its description.
    Rooted<Value> key{Value::fromSymbol(patternSymbolKey(which))};
    const Value found = Value(bronze_elem_get(arg.get().rawBits(), key.get().rawBits()));
    if (rtExceptionPending()) return false;
    // GetMethod step 3: both absent spellings mean "no method", and only then
    // does the caller run its own algorithm.
    if (found.isUndefined() || found.isNull()) return false;
    if (!isCallable(found)) {
        rtThrowTypeError(std::string("a pattern argument's [Symbol.") + patternSymbolName(which) +
                         "] is not callable");
        return false;
    }
    out.set(found);
    return true;
}

Value rtCallPatternMethod(Rooted<Value>& method, Rooted<Value>& receiver, Rooted<Value>& first,
                          Rooted<Value>& second, uint32_t argCount) {
    // The RECEIVER is the pattern argument, not the string: 22.1.3 calls the
    // method it found ON the object it found it on, so `[Symbol.replace]`
    // reaches its own state through `this`.
    //
    // The block is filled from roots on the statement before the call and
    // `bronze_dynamic_call` allocates nothing on its way to the callee, so
    // nothing can move between the reads and the entry.
    Value block[2];
    block[0] = first.get();
    block[1] = second.get();
    return Value(bronze_dynamic_call(method.get().rawBits(), receiver.get().rawBits(), argCount,
                                     reinterpret_cast<const uint64_t*>(block)));
}

uint64_t rtStringSplitWithRegExp(uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!requireString(Value(thisBits), "split", selfVal)) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> self{selfVal};
    Rooted<Value> re{args[0]};
    return rtRegExpSplit(re, self, args[1]).rawBits();
}

}  // namespace bronze::runtime
