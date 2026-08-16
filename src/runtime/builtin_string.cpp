// String.prototype's plain members: ordinary function objects over native code
// pointers, every one of them opening with the RootedArgs prologue
// (rt_roots.h). The table at the foot of the file is installed as
// non-enumerable own properties of the `String.prototype` OBJECT that
// builtin_wrappers.cpp builds — a program can hold them, compare them and pass
// them to `.call`, where an array's members are still answered beside the value
// by the property path.
//
// Strings are immutable, so every member here allocates a fresh string and none
// of them can work in place. They are also stored in one of two representations
// — Latin-1 or UTF-16 — so the shared currency below is a vector of UTF-16 code
// units, rebuilt into whichever representation fits the result. That is a copy
// per operation, and it is the honest starting point: a
// representation-specialized fast path is worth writing when a benchmark asks
// for one, not before.
//
// Case mapping is the one member family whose answer is not computable from the
// characters alone: it needs the Unicode case tables, which are generated into
// unicode_case_data_*.cpp and applied by unicode_case.cpp. FULL mapping, so
// `toUpperCase` can return a longer string than it was given.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/number_format.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/unicode_case.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

using Units = std::vector<uint16_t>;

Units unitsOf(const StringHeader* s) { return rtStringUnits(s); }

Value stringFromUnits(const Units& units) { return rtStringFromUnits(units); }

Units thisUnits(Value self, const char* method) {
    // 22.1.3.35 thisStringValue: a primitive string, or a String OBJECT's
    // [[StringData]] — `String.prototype.indexOf.call(new String("ab"), "b")`
    // searches the characters the wrapper wraps, which is the whole reason the
    // wrapper is worth having a slot for.
    Value str;
    if (!rtThisStringValue(self, str)) {
        // 22.1.3's RequireObjectCoercible plus ToString: a String.prototype
        // method reached with a `this` that is neither is a TypeError, and
        // since a catchable one. The empty unit sequence is what the caller
        // then computes over, and its result is discarded — the cell is already
        // set, so its caller's test fires before the value is read.
        rtThrowTypeError(std::string("String.prototype.") + method +
                         " called on a value that is not a string");
        return Units{};
    }
    return unitsOf(str.asString<StringHeader>());
}

// ToString of an argument, as units. Every search/replace member coerces its
// argument this way (`"abc".includes(1)` searches for "1").
Units argUnits(Value v) {
    Value str = rtValueToString(v);
    return unitsOf(str.asString<StringHeader>());
}

// 7.1.6 ToUint32. `split`'s limit is the one member here that takes it: the
// argument wraps modulo 2^32 rather than clamping, so a limit of -1 is
// 4294967295 (no limit in practice) and 2^32 is 0 (an empty result).
uint32_t toUint32(Value v) {
    const double n = rtToNumber(v);
    if (rtExceptionPending() || !std::isfinite(n) || n == 0.0) return 0;
    const double truncated = std::trunc(n);
    const double wrapped = std::fmod(truncated, 4294967296.0);
    return static_cast<uint32_t>(static_cast<int64_t>(wrapped < 0 ? wrapped + 4294967296.0 : wrapped));
}

double toInteger(double d) {
    if (std::isnan(d)) return 0.0;
    if (std::isinf(d)) return d;
    double t = std::trunc(d);
    return t == 0.0 ? 0.0 : t;
}

uint32_t relativeIndex(double rel, size_t len) {
    if (rel < 0) {
        double from = static_cast<double>(len) + rel;
        return from < 0 ? 0u : static_cast<uint32_t>(from);
    }
    double capped = std::min(rel, static_cast<double>(len));
    return static_cast<uint32_t>(capped);
}

// A clamp with no negative meaning, which is what substring uses (and the
// reason substring and slice disagree on negative arguments).
uint32_t clampIndex(double v, size_t len) {
    if (std::isnan(v) || v < 0) return 0;
    if (v > static_cast<double>(len)) return static_cast<uint32_t>(len);
    return static_cast<uint32_t>(v);
}

// ECMA-262 WhiteSpace + LineTerminator, which is what trim strips — a wider set
// than isspace(), and one that must not depend on a locale.
bool isTrimmable(uint16_t u) {
    switch (u) {
        case 0x0009: case 0x000A: case 0x000B: case 0x000C: case 0x000D:
        case 0x0020: case 0x00A0: case 0x1680: case 0x2028: case 0x2029:
        case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
            return true;
        default:
            return u >= 0x2000 && u <= 0x200A;
    }
}

// Index of `needle` in `hay` at or after `from`, or -1. The empty needle
// matches at `from`, which is what makes "abc".indexOf("") == 0.
int64_t indexOfUnits(const Units& hay, const Units& needle, size_t from) {
    if (needle.size() > hay.size()) return -1;
    for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
        if (std::equal(needle.begin(), needle.end(), hay.begin() + i)) {
            return static_cast<int64_t>(i);
        }
    }
    return -1;
}

// ---- simple readers -------------------------------------------------------

uint64_t stringCharAt(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "charAt");
    double idx = toInteger(rtToNumber(args.at(0, Value::fromDouble(0.0))));
    if (idx < 0 || idx >= static_cast<double>(self.size())) {
        return stringFromUnits({}).rawBits();  // out of range is "", not undefined
    }
    Units one{self[static_cast<size_t>(idx)]};
    return stringFromUnits(one).rawBits();
}

uint64_t stringAt(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "at");
    double rel = toInteger(rtToNumber(args.at(0, Value::fromDouble(0.0))));
    double idx = rel < 0 ? static_cast<double>(self.size()) + rel : rel;
    if (idx < 0 || idx >= static_cast<double>(self.size())) {
        return Value::fromUndefined().rawBits();  // out of range is undefined, unlike charAt
    }
    Units one{self[static_cast<size_t>(idx)]};
    return stringFromUnits(one).rawBits();
}

// Out of range is NaN, not 0: 0 is a real code unit (NUL), so answering it
// for a missing position is indistinguishable from finding one.
//
// Reads the header directly rather than through `thisUnits`, because this is
// the one member that needs a single unit and not the whole string — copying
// the string to read one character of it is a cost `for (i…) s.charCodeAt(i)`
// would pay per iteration.
uint64_t stringCharCodeAt(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self;
    if (!rtThisStringValue(Value(thisBits), self)) {
        return rtThrowTypeError(
                   "String.prototype.charCodeAt called on a value that is not a string")
            .rawBits();
    }
    // The index conversion runs 7.1.4 and so may call user code, which
    // allocates and can move the string; the header is therefore taken AFTER
    // it, out of a rooted slot the collector updates.
    Rooted<Value> str{self};
    double idx = toInteger(rtToNumber(args.at(0, Value::fromDouble(0.0))));
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    const StringHeader* s = str.get().asString<StringHeader>();
    if (idx < 0 || idx >= static_cast<double>(s->getLength())) {
        return Value::fromDouble(std::numeric_limits<double>::quiet_NaN()).rawBits();
    }
    return Value::fromDouble(s->charCodeAt(static_cast<uint32_t>(idx))).rawBits();
}

// Out of range is `undefined` here and NaN in charCodeAt above. That is
// 22.1.3.4 step 4 rather than an inconsistency between the two: charCodeAt
// answers a Number at every position it accepts, so it has to answer one
// where it accepts none, and codePointAt is free to say "no code point here".
//
// The pairing rule is 11.1.4 CodePointAt. A leading surrogate followed by a
// trailing one is ONE code point; an unpaired surrogate is its own code unit,
// which covers three cases that all read the same way — a lead at the very end
// of the string, a lead not followed by a trail, and a trail read directly.
// Reading at the second half of a pair therefore answers that half and not the
// pair, which is what makes this the member that walks a string by code point.
uint64_t stringCodePointAt(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self;
    if (!rtThisStringValue(Value(thisBits), self)) {
        return rtThrowTypeError(
                   "String.prototype.codePointAt called on a value that is not a string")
            .rawBits();
    }
    // As in charCodeAt: the header is only valid after the index conversion,
    // which may run user code and move the string.
    Rooted<Value> str{self};
    double idx = toInteger(rtToNumber(args.at(0, Value::fromDouble(0.0))));
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    const StringHeader* s = str.get().asString<StringHeader>();
    const uint32_t size = s->getLength();
    if (idx < 0 || idx >= static_cast<double>(size)) return Value::fromUndefined().rawBits();
    const uint32_t at = static_cast<uint32_t>(idx);
    const uint32_t first = s->charCodeAt(at);
    if (first < 0xD800 || first > 0xDBFF || at + 1 == size) {
        return Value::fromDouble(first).rawBits();
    }
    const uint32_t second = s->charCodeAt(at + 1);
    if (second < 0xDC00 || second > 0xDFFF) return Value::fromDouble(first).rawBits();
    return Value::fromDouble((first - 0xD800) * 0x400 + (second - 0xDC00) + 0x10000).rawBits();
}

uint64_t stringIndexOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "indexOf");
    Units needle = argUnits(args[0]);
    uint32_t from = args.count() > 1 ? clampIndex(toInteger(rtToNumber(args[1])), self.size()) : 0;
    return Value::fromDouble(static_cast<double>(indexOfUnits(self, needle, from))).rawBits();
}

uint64_t stringLastIndexOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "lastIndexOf");
    Units needle = argUnits(args[0]);
    int64_t found = -1;
    for (int64_t at = indexOfUnits(self, needle, 0); at >= 0;
         at = indexOfUnits(self, needle, static_cast<size_t>(at) + 1)) {
        found = at;
        if (static_cast<size_t>(at) + 1 > self.size()) break;
    }
    return Value::fromDouble(static_cast<double>(found)).rawBits();
}

uint64_t stringIncludes(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "includes");
    Units needle = argUnits(args[0]);
    uint32_t from = args.count() > 1 ? clampIndex(toInteger(rtToNumber(args[1])), self.size()) : 0;
    return Value::fromBool(indexOfUnits(self, needle, from) >= 0).rawBits();
}

uint64_t stringStartsWith(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "startsWith");
    Units needle = argUnits(args[0]);
    uint32_t from = args.count() > 1 ? clampIndex(toInteger(rtToNumber(args[1])), self.size()) : 0;
    bool ok = from + needle.size() <= self.size() &&
              std::equal(needle.begin(), needle.end(), self.begin() + from);
    return Value::fromBool(ok).rawBits();
}

uint64_t stringEndsWith(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "endsWith");
    Units needle = argUnits(args[0]);
    size_t end = args.count() > 1 && !args[1].isUndefined()
                     ? clampIndex(toInteger(rtToNumber(args[1])), self.size())
                     : self.size();
    bool ok = needle.size() <= end &&
              std::equal(needle.begin(), needle.end(), self.begin() + (end - needle.size()));
    return Value::fromBool(ok).rawBits();
}

// ---- producers ------------------------------------------------------------

uint64_t stringSlice(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "slice");
    uint32_t start = args.count() > 0 ? relativeIndex(toInteger(rtToNumber(args[0])), self.size()) : 0;
    uint32_t end = args.count() > 1 && !args[1].isUndefined()
                       ? relativeIndex(toInteger(rtToNumber(args[1])), self.size())
                       : static_cast<uint32_t>(self.size());
    if (end < start) end = start;
    return stringFromUnits(Units(self.begin() + start, self.begin() + end)).rawBits();
}

uint64_t stringSubstring(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "substring");
    uint32_t a = args.count() > 0 ? clampIndex(toInteger(rtToNumber(args[0])), self.size()) : 0;
    uint32_t b = args.count() > 1 && !args[1].isUndefined()
                     ? clampIndex(toInteger(rtToNumber(args[1])), self.size())
                     : static_cast<uint32_t>(self.size());
    // substring SWAPS its arguments when they are the wrong way round;
    // slice, one function up, answers "" instead. That difference is the
    // whole reason both exist.
    if (a > b) std::swap(a, b);
    return stringFromUnits(Units(self.begin() + a, self.begin() + b)).rawBits();
}

// Annex B.2.3.1 String.prototype.substr(start, length)
uint64_t stringSubstr(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "substr");
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    const size_t size = self.size();
    double start = args.count() > 0 ? toInteger(rtToNumber(args[0])) : 0.0;
    if (std::isinf(start) && start < 0.0) {
        start = 0.0;
    } else if (start < 0.0) {
        start = std::max(static_cast<double>(size) + start, 0.0);
    } else {
        start = std::min(start, static_cast<double>(size));
    }
    double len = args.count() > 1 && !args[1].isUndefined() ? toInteger(rtToNumber(args[1]))
                                                           : static_cast<double>(size);
    if (len <= 0.0 || std::isnan(len)) return rtMakeString("").rawBits();
    uint32_t intStart = static_cast<uint32_t>(start);
    uint32_t intLength = static_cast<uint32_t>(std::min(len, static_cast<double>(size - intStart)));
    return stringFromUnits(Units(self.begin() + intStart, self.begin() + intStart + intLength)).rawBits();
}

uint64_t stringConcat(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units out = thisUnits(Value(thisBits), "concat");
    for (uint32_t i = 0; i < args.count(); ++i) {
        Units piece = argUnits(args[i]);
        out.insert(out.end(), piece.begin(), piece.end());
    }
    return stringFromUnits(out).rawBits();
}

uint64_t stringRepeat(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "repeat");
    double n = toInteger(rtToNumber(args[0]));
    if (n < 0 || std::isinf(n)) {
        // 22.1.3.16 step 4: a RangeError, not a clamp.
        // The count is reported the way JS spells a number, not the way
        // printf does: std::to_string(-1.0) is "-1.000000".
        char buf[32];
        const size_t len = formatJsNumber(n, buf);
        return rtThrowRangeError("Invalid count value: " + std::string(buf, len)).rawBits();
    }
    Units out;
    out.reserve(self.size() * static_cast<size_t>(n));
    for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i) {
        out.insert(out.end(), self.begin(), self.end());
    }
    return stringFromUnits(out).rawBits();
}

template <bool Start, bool End>
uint64_t stringTrimImpl(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "trim");
    size_t a = 0;
    size_t b = self.size();
    if constexpr (Start) {
        while (a < b && isTrimmable(self[a])) ++a;
    }
    if constexpr (End) {
        while (b > a && isTrimmable(self[b - 1])) --b;
    }
    return stringFromUnits(Units(self.begin() + a, self.begin() + b)).rawBits();
}

template <bool AtStart>
uint64_t stringPadImpl(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "pad");
    double target = toInteger(rtToNumber(args[0]));
    if (target <= static_cast<double>(self.size())) return stringFromUnits(self).rawBits();
    Units filler = args.count() > 1 && !args[1].isUndefined() ? argUnits(args[1]) : Units{' '};
    if (filler.empty()) return stringFromUnits(self).rawBits();

    Units pad;
    while (pad.size() + self.size() < static_cast<size_t>(target)) {
        pad.push_back(filler[pad.size() % filler.size()]);
    }
    Units out;
    if constexpr (AtStart) {
        out = pad;
        out.insert(out.end(), self.begin(), self.end());
    } else {
        out = self;
        out.insert(out.end(), pad.begin(), pad.end());
    }
    return stringFromUnits(out).rawBits();
}

// 22.1.3.28 toUpperCase / 22.1.3.30 toLowerCase, which are both 11.1.3 over the
// Default Case Conversion tables (unicode_case.{h,cpp}). FULL, so the result
// can be LONGER than the input: "\u00df".toUpperCase() is "SS" and
// "\u0130".toLowerCase() is two code points. Nothing here is per-character, and
// that is why the whole string goes to one function rather than a loop over
// units living in this file.
//
// The toLocale* twins share this body, and now they share it as an ANSWER
// rather than as a shared refusal. 22.1.3.26 and 22.1.3.27 say a locale
// tailoring is applied "in an implementation-defined locale-sensitive way", and
// with no locale to be sensitive to, what is left is the language-independent
// mapping — which is what the tables hold, because the generator drops every
// SpecialCasing line carrying a language ID. So `"i".toLocaleUpperCase()` is
// "I" here and stays "I": the Turkish tailoring that would make it "\u0130" is
// data bronze deliberately does not carry, not data it has and ignores.
template <bool Upper, bool Locale = false>
uint64_t stringCaseImpl(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits),
                           Locale ? (Upper ? "toLocaleUpperCase" : "toLocaleLowerCase")
                                  : (Upper ? "toUpperCase" : "toLowerCase"));
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Units out = Upper ? unicode::toUpperFull(self) : unicode::toLowerFull(self);
    return stringFromUnits(out).rawBits();
}

uint64_t stringSplit(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "split");
    if (args[0].isObject()) {
        // A RegExp separator is a different algorithm entirely (22.2.6.14
        // SplitMatcher, which also yields the separator's captures), so it is
        // handed to the module that owns the matcher.
        if (rtIsRegExp(args[0])) return rtStringSplitWithRegExp(thisBits, argc, argv);
        fatal("unsupported: String.prototype.split with a separator that is neither a string "
              "nor a RegExp is not implemented");
    }

    // Step 4, and it runs BEFORE the separator is looked at — which is the
    // whole reason `"abc".split(undefined, 0)` is `[]` and not `["abc"]`: the
    // limit-zero exit (step 6) comes ahead of the undefined-separator exit
    // (step 7). `undefined` is 2^32-1, everything else is ToUint32.
    const uint32_t limit = args[1].isUndefined() ? 0xFFFFFFFFu : toUint32(args[1]);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    ArrayHeader* raw = ArrayHeader::create(rtHeap(), 4);
    raw->length = 0;
    Rooted<Value> out{Value::fromObject(raw)};

    // Answers false once the array has reached the limit, so every caller stops
    // at the same place rather than each remembering to check.
    auto pushPiece = [&](const Units& piece) {
        if (out.get().asObject<ArrayHeader>()->length >= limit) return false;
        Rooted<Value> val{stringFromUnits(piece)};
        uint32_t at = out.get().asObject<ArrayHeader>()->length;
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at, val);
        return true;
    };

    if (limit == 0) return out.get().rawBits();

    // No separator at all: the whole string, as one element. Distinct from
    // an empty separator, which splits into single code units.
    if (args[0].isUndefined()) {
        pushPiece(self);
        return out.get().rawBits();
    }
    Units sep = argUnits(args[0]);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    if (sep.empty()) {
        // Step 9: the first `min(len, lim)` code units, each its own element.
        // An empty subject yields nothing here, which is why `"".split("")` is
        // `[]` where `"".split("x")` is `[""]` — the two exits are steps 9 and
        // 10 and they disagree on purpose.
        for (uint16_t u : self) {
            if (!pushPiece(Units{u})) break;
        }
        return out.get().rawBits();
    }

    size_t at = 0;
    for (;;) {
        int64_t found = indexOfUnits(self, sep, at);
        if (found < 0) break;
        if (!pushPiece(Units(self.begin() + at, self.begin() + found))) {
            return out.get().rawBits();
        }
        at = static_cast<size_t>(found) + sep.size();
    }
    pushPiece(Units(self.begin() + at, self.end()));
    return out.get().rawBits();
}

// 22.1.3.28 toString and 22.1.3.35 valueOf are the SAME operation —
// thisStringValue — which is why one function answers both names. For a String
// object that is the [[StringData]] slot, and it is the step that makes
// `String(new String("ab"))` and `new String("ab") + ""` the characters rather
// than a named ToPrimitive error.
uint64_t stringItself(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self;
    if (!rtThisStringValue(Value(thisBits), self)) {
        return rtThrowTypeError(
                   "String.prototype.toString called on a value that is not a string")
            .rawBits();
    }
    return self.rawBits();
}

// 22.1.3.11 isWellFormed
uint64_t stringIsWellFormed(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Units self = thisUnits(Value(thisBits), "isWellFormed");
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    const size_t len = self.size();
    for (size_t i = 0; i < len; ++i) {
        const uint16_t c = self[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 < len && self[i + 1] >= 0xDC00 && self[i + 1] <= 0xDFFF) {
                ++i;
            } else {
                return Value::fromBool(false).rawBits();
            }
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            return Value::fromBool(false).rawBits();
        }
    }
    return Value::fromBool(true).rawBits();
}

// 22.1.3.31 toWellFormed
uint64_t stringToWellFormed(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value selfVal(thisBits);
    Units self = thisUnits(selfVal, "toWellFormed");
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    const size_t len = self.size();
    bool hasLone = false;
    for (size_t i = 0; i < len; ++i) {
        const uint16_t c = self[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 < len && self[i + 1] >= 0xDC00 && self[i + 1] <= 0xDFFF) {
                ++i;
            } else {
                hasLone = true;
                break;
            }
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            hasLone = true;
            break;
        }
    }
    if (!hasLone) {
        Value str;
        rtThisStringValue(selfVal, str);
        return str.rawBits();
    }
    Units out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const uint16_t c = self[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 < len && self[i + 1] >= 0xDC00 && self[i + 1] <= 0xDFFF) {
                out.push_back(c);
                out.push_back(self[i + 1]);
                ++i;
            } else {
                out.push_back(0xFFFD);
            }
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            out.push_back(0xFFFD);
        } else {
            out.push_back(c);
        }
    }
    return stringFromUnits(out).rawBits();
}

// 22.1.3.16 normalize
uint64_t stringNormalize(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // Rooted, because this is the one member that answers with its RECEIVER
    // rather than with a freshly built string: the form argument's ToString can
    // be user code, and an unrooted receiver read afterwards is a moved one.
    Rooted<Value> selfVal{Value(thisBits)};
    thisUnits(selfVal.get(), "normalize");
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    std::string form = "NFC";
    if (args.count() > 0 && !args[0].isUndefined()) {
        Rooted<Value> formVal{rtValueToString(args[0])};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        form = rtUtf8Chars(formVal.get().asString<StringHeader>());
    }
    if (form != "NFC" && form != "NFD" && form != "NFKC" && form != "NFKD") {
        return rtThrowRangeError(
                   "The normalization form should be one of 'NFC', 'NFD', 'NFKC', 'NFKD'")
            .rawBits();
    }

    Value str;
    rtThisStringValue(selfVal.get(), str);
    return str.rawBits();
}

const NativeMethod kStringMethods[] = {
    {"at", stringAt, 1},
    {"charAt", stringCharAt, 1},
    {"charCodeAt", stringCharCodeAt, 1},
    {"codePointAt", stringCodePointAt, 1},
    {"concat", stringConcat, 0},
    {"endsWith", stringEndsWith, 1},
    {"includes", stringIncludes, 1},
    {"indexOf", stringIndexOf, 1},
    {"isWellFormed", stringIsWellFormed, 0},
    {"lastIndexOf", stringLastIndexOf, 1},
    {"normalize", stringNormalize, 0},
    {"padEnd", stringPadImpl<false>, 1},
    {"padStart", stringPadImpl<true>, 1},
    {"repeat", stringRepeat, 1},
    {"slice", stringSlice, 0},
    {"split", stringSplit, 0},
    {"startsWith", stringStartsWith, 1},
    {"substr", stringSubstr, 2},
    {"substring", stringSubstring, 0},
    {"toLocaleLowerCase", stringCaseImpl<false, true>, 0},
    {"toLocaleUpperCase", stringCaseImpl<true, true>, 0},
    {"toLowerCase", stringCaseImpl<false>, 0},
    {"toString", stringItself, 0},
    {"toWellFormed", stringToWellFormed, 0},
    {"toUpperCase", stringCaseImpl<true>, 0},
    {"trim", stringTrimImpl<true, true>, 0},
    {"trimEnd", stringTrimImpl<false, true>, 0},
    {"trimStart", stringTrimImpl<true, false>, 0},
    {"valueOf", stringItself, 0},
};

}  // namespace

std::vector<uint16_t> rtStringUnits(const StringHeader* s) {
    std::vector<uint16_t> out;
    const uint32_t len = s->getLength();
    out.reserve(len);
    if (s->isLatin1()) {
        const char* data = s->latin1Data();
        for (uint32_t i = 0; i < len; ++i) {
            out.push_back(static_cast<unsigned char>(data[i]));
        }
    } else {
        const uint16_t* data = s->utf16Data();
        out.insert(out.end(), data, data + len);
    }
    return out;
}

// Latin-1 when every unit fits, so an ASCII result of an ASCII input stays
// in the compact representation instead of doubling in size.
Value rtStringFromUnits(const std::vector<uint16_t>& units) {
    bool latin1 = true;
    for (uint16_t u : units) {
        if (u > 0xFF) {
            latin1 = false;
            break;
        }
    }
    if (latin1) {
        std::string bytes;
        bytes.reserve(units.size());
        for (uint16_t u : units) bytes.push_back(static_cast<char>(u));
        return Value::fromString(StringHeader::createLatin1(rtHeap(), bytes.data(),
                                                            static_cast<uint32_t>(bytes.size())));
    }
    return Value::fromString(
        StringHeader::createUTF16(rtHeap(), units.data(), static_cast<uint32_t>(units.size())));
}

// The members above, onto the `String.prototype` object builtin_wrappers.cpp
// builds. The members that take a PATTERN install themselves from their own
// translation unit, so the split between the two files is invisible to a
// program: both halves land on one object.
void rtInstallStringMethods(Rooted<Value>& proto) {
    rtDefineMethods(proto, kStringMethods, std::size(kStringMethods));
}

}  // namespace bronze::runtime
