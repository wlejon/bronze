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
#include "runtime/builtin_string_internal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/locale_hooks.h"
#include "runtime/native_fast.h"
#include "runtime/object.h"
#include "runtime/number_format.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/unicode_case.h"
#include "runtime/unicode_norm.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

using Units = std::vector<uint16_t>;
using string_detail::toInteger;

Units unitsOf(const StringHeader* s) { return rtStringUnits(s); }

Value stringFromUnits(const Units& units) { return rtStringFromUnits(units); }

Units thisUnits(Value self, const char* method) {
    // 22.1.3's RequireObjectCoercible plus ToString: `String.prototype.
    // toUpperCase.call(123)` is "123", a String object reads as its characters
    // through its toString, and only null or undefined is a TypeError.
    Value str;
    if (!rtStringMethodThis(self, str)) {
        rtThrowTypeError(std::string("String.prototype.") + method +
                         " called on null or undefined");
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
    if (!std::isfinite(n) || n == 0.0) return 0;
    const double truncated = std::trunc(n);
    const double wrapped = std::fmod(truncated, 4294967296.0);
    return static_cast<uint32_t>(static_cast<int64_t>(wrapped < 0 ? wrapped + 4294967296.0 : wrapped));
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

}  // namespace

namespace {


uint64_t stringConcat(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self(thisBits);
    if (self.isUndefined() || self.isNull()) {
        return rtThrowTypeError("String.prototype.concat called on null or undefined").rawBits();
    }
    Value thisStr = rtValueToString(self);
    Units out = unitsOf(thisStr.asString<StringHeader>());
    for (uint32_t i = 0; i < args.count(); ++i) {
        Units piece = argUnits(args[i]);
        out.insert(out.end(), piece.begin(), piece.end());
    }
    return stringFromUnits(out).rawBits();
}

constexpr double kMaxStringLength = 134217727.0;

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
    if (n == 0.0 || self.empty()) return rtMakeString("").rawBits();
    if (n * static_cast<double>(self.size()) > kMaxStringLength) {
        return rtThrowRangeError("Invalid string length").rawBits();
    }
    Units out;
    const auto count = static_cast<uint32_t>(n);
    out.reserve(self.size() * static_cast<size_t>(count));
    for (uint32_t i = 0; i < count; ++i) {
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
    if (target > kMaxStringLength) {
        return rtThrowRangeError("Invalid string length").rawBits();
    }

    Units pad;
    const auto targetLen = static_cast<size_t>(target);
    while (pad.size() + self.size() < targetLen) {
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
    // A string primitive that is already in the case asked for is its own
    // answer: a string value has no identity a program can tell apart from an
    // equal one, so returning it is returning the converted copy. Asked only
    // of ASCII, where the mapping is one unit to one unit and the only units
    // that change are the 26 letters of the other case — three.js's
    // `generateUUID` lowercases a string built from lowercase hex digits for
    // every object it makes.
    if (Value self(thisBits); self.isString()) {
        const StringHeader* str = self.asString<StringHeader>();
        if (str->isLatin1() && !str->isBuilder()) {
            const auto* units = reinterpret_cast<const unsigned char*>(str->latin1Data());
            bool unchanged = true;
            for (uint32_t i = 0; i < str->length && unchanged; ++i) {
                const unsigned char c = units[i];
                unchanged = c < 0x80 && !(Upper ? (c >= 'a' && c <= 'z') : (c >= 'A' && c <= 'Z'));
            }
            if (unchanged) return thisBits;
        }
    }
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits),
                           Locale ? (Upper ? "toLocaleUpperCase" : "toLocaleLowerCase")
                                  : (Upper ? "toUpperCase" : "toLowerCase"));
    Units out = Upper ? unicode::toUpperFull(self) : unicode::toLowerFull(self);
    return stringFromUnits(out).rawBits();
}

uint64_t stringSplit(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // Step 1 only: step 2 hands the receiver UNCONVERTED to a separator's
    // [Symbol.split], and the ToString of step 3 waits until after it.
    Rooted<Value> receiver{Value(thisBits)};
    if (receiver.get().isNull() || receiver.get().isUndefined()) {
        rtThrowTypeError("String.prototype.split called on null or undefined");
    }
    if (args[0].isObject()) {
        // 22.1.3.23 step 2: a separator carrying its own `[Symbol.split]`
        // decides the whole algorithm, and the literal-string walk below never
        // runs. Only an OBJECT separator can carry one — a string reaches
        // `String.prototype`, which defines none — which is why the dispatch
        // sits inside this branch and costs `"a,b".split(",")` nothing.
        Rooted<Value> separator{args[0]};
        Rooted<Value> splitter;
        if (rtPatternMethod(separator, PatternSymbol::Split, splitter)) {
            // Step 2.b passes the LIMIT along, so the method sees both
            // arguments the call site wrote.
            Rooted<Value> limit{args[1]};
            return rtCallPatternMethod(splitter, separator, receiver, limit, /*argCount=*/2)
                .rawBits();
        }
        // A RegExp separator is a different algorithm entirely (22.2.6.14
        // SplitMatcher, which also yields the separator's captures), so it is
        // handed to the module that owns the matcher.
        if (rtIsRegExp(args[0])) {
            Rooted<Value> str{rtValueToString(receiver.get())};
            return rtStringSplitWithRegExp(str.get().rawBits(), argc, argv);
        }
        // An object separator lacking [Symbol.split] falls through to
        // ToString(separator) below per 22.1.3.23.
    }
    Units self = thisUnits(receiver.get(), "split");

    // Step 4, and it runs BEFORE the separator is looked at — which is the
    // whole reason `"abc".split(undefined, 0)` is `[]` and not `["abc"]`: the
    // limit-zero exit (step 6) comes ahead of the undefined-separator exit
    // (step 7). `undefined` is 2^32-1, everything else is ToUint32.
    const uint32_t limit = args[1].isUndefined() ? 0xFFFFFFFFu : toUint32(args[1]);

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
// thisStringValue. For a String object that is the [[StringData]] slot, and
// it is the step that makes `String(new String("ab"))` and `new String("ab") +
// ""` the characters rather than a named ToPrimitive error. Two bodies all the
// same, because they are two function OBJECTS: interning is by code pointer,
// and one body would make `toString === valueOf` true and give `valueOf` the
// wrong `name`.
uint64_t thisStringValueOf(uint64_t thisBits, const char* method) {
    Value self;
    if (!rtThisStringValue(Value(thisBits), self)) {
        return rtThrowTypeError(std::string("String.prototype.") + method +
                                " called on a value that is not a string")
            .rawBits();
    }
    return self.rawBits();
}

uint64_t stringToString(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    return thisStringValueOf(thisBits, "toString");
}

uint64_t stringValueOf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    return thisStringValueOf(thisBits, "valueOf");
}

// 22.1.3.11 isWellFormed
uint64_t stringIsWellFormed(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Units self = thisUnits(Value(thisBits), "isWellFormed");
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
    // Converted once and rooted: the receiver may be an object whose toString
    // runs user code, and the answer below can be the converted string itself.
    Value coerced;
    if (!rtStringMethodThis(Value(thisBits), coerced)) {
        rtThrowTypeError("String.prototype.toWellFormed called on null or undefined");
    }
    Rooted<Value> str{coerced};
    Units self = unitsOf(str.get().asString<StringHeader>());
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
    if (!hasLone) return str.get().rawBits();
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
    Value coerced;
    if (!rtStringMethodThis(Value(thisBits), coerced)) {
        rtThrowTypeError("String.prototype.normalize called on null or undefined");
    }
    Rooted<Value> selfVal{coerced};

    std::string form = "NFC";
    if (args.count() > 0 && !args[0].isUndefined()) {
        Rooted<Value> formVal{rtValueToString(args[0])};
        form = rtUtf8Chars(formVal.get().asString<StringHeader>());
    }
    unicode::NormForm nf = unicode::NormForm::NFC;
    if (form == "NFD") {
        nf = unicode::NormForm::NFD;
    } else if (form == "NFKC") {
        nf = unicode::NormForm::NFKC;
    } else if (form == "NFKD") {
        nf = unicode::NormForm::NFKD;
    } else if (form != "NFC") {
        return rtThrowRangeError(
                   "The normalization form should be one of 'NFC', 'NFD', 'NFKC', 'NFKD'")
            .rawBits();
    }

    // Nothing allocates between here and the quick check, so the header is
    // read in place: a string whose every unit is under the form's bound is
    // already normalized, and the answer is the receiver itself -- which is
    // most strings, and every Latin-1 one under NFC.
    const Value str = selfVal.get();
    const StringHeader* header = str.asString<StringHeader>();
    const bool trivial =
        header->isLatin1()
            ? unicode::isTriviallyNormalizedLatin1(header->latin1Data(), header->getLength(), nf)
            : unicode::isTriviallyNormalized(header->utf16Data(), header->getLength(), nf);
    if (trivial) return str.rawBits();

    const Units self = unitsOf(header);
    const Units out = unicode::normalize(self, nf);
    // Still nothing on the JS heap has been allocated, so `str` is current.
    if (out == self) return str.rawBits();
    return stringFromUnits(out).rawBits();
}

uint64_t stringLocaleCompare(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    // A host's Intl collates it (locale_hooks.h); without one, code-unit order.
    if (uint64_t out = 0; rtRunLocaleHook(LocaleHook::StringLocaleCompare, thisBits, argc, argv, out)) {
        return out;
    }
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "localeCompare");
    Units that = args.count() > 0 ? argUnits(args[0]) : argUnits(Value::fromUndefined());

    size_t minLen = std::min(self.size(), that.size());
    for (size_t i = 0; i < minLen; ++i) {
        if (self[i] < that[i]) return Value::fromDouble(-1.0).rawBits();
        if (self[i] > that[i]) return Value::fromDouble(1.0).rawBits();
    }
    if (self.size() < that.size()) return Value::fromDouble(-1.0).rawBits();
    if (self.size() > that.size()) return Value::fromDouble(1.0).rawBits();
    return Value::fromDouble(0.0).rawBits();
}

const NativeMethod kStringMethods[] = {
    {"at", string_detail::stringAt, 1, 1},
    {"charAt", bronze_string_char_at, 1, 1},
    {"charCodeAt", bronze_string_char_code_at, 1, 1},
    {"codePointAt", string_detail::stringCodePointAt, 1, 1},
    {"concat", stringConcat, 0, 1},
    {"endsWith", string_detail::stringEndsWith, 1, 1},
    {"includes", string_detail::stringIncludes, 1, 1},
    {"indexOf", bronze_string_index_of, 1, 1},
    {"isWellFormed", stringIsWellFormed, 0, 0},
    {"lastIndexOf", string_detail::stringLastIndexOf, 1, 1},
    {"localeCompare", stringLocaleCompare, 1, 1},
    {"normalize", stringNormalize, 0, 0},
    {"padEnd", stringPadImpl<false>, 1, 1},
    {"padStart", stringPadImpl<true>, 1, 1},
    {"repeat", stringRepeat, 1, 1},
    {"slice", bronze_string_slice, 0, 2},
    {"split", stringSplit, 0, 2},
    {"startsWith", bronze_string_starts_with, 1, 1},
    {"substr", string_detail::stringSubstr, 2, 2},
    {"substring", bronze_string_substring, 0, 2},
    {"toLocaleLowerCase", stringCaseImpl<false, true>, 0, 0},
    {"toLocaleUpperCase", stringCaseImpl<true, true>, 0, 0},
    {"toLowerCase", stringCaseImpl<false>, 0, 0},
    {"toString", stringToString, 0, 0},
    {"toWellFormed", stringToWellFormed, 0, 0},
    {"toUpperCase", stringCaseImpl<true>, 0, 0},
    {"trim", stringTrimImpl<true, true>, 0, 0},
    {"trimEnd", stringTrimImpl<false, true>, 0, 0},
    {"trimStart", stringTrimImpl<true, false>, 0, 0},
    {"valueOf", stringValueOf, 0, 0},
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
