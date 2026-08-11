// String.prototype. Same shape as builtin_array.cpp: ordinary function
// objects over native code pointers, handed out by the property path before
// it consults the unimplemented-member table, and every one of them opens
// with the RootedArgs prologue (rt_internal.h).
//
// Strings are immutable (docs/0004), so every member here allocates a fresh
// string and none of them can work in place. They are also stored in one of
// two representations — Latin-1 or UTF-16 — so the shared currency below is
// a vector of UTF-16 code units, rebuilt into whichever representation fits
// the result. That is a copy per operation, and it is the honest starting
// point: a representation-specialized fast path is worth writing when a
// benchmark asks for one, not before.
//
// Where a correct answer needs Unicode tables bronze does not carry (case
// mapping past ASCII) the call is a hard error naming itself, never a
// quietly wrong answer.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/array.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

using Units = std::vector<uint16_t>;

Units unitsOf(const StringHeader* s) {
    Units out;
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
Value stringFromUnits(const Units& units) {
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

Units thisUnits(Value self, const char* method) {
    if (!self.isString()) {
        fatal((std::string("String.prototype.") + method +
               " called on a value that is not a string")
                  .c_str());
    }
    return unitsOf(self.asString<StringHeader>());
}

// ToString of an argument, as units. Every search/replace member coerces its
// argument this way (`"abc".includes(1)` searches for "1").
Units argUnits(Value v) {
    Value str = rtValueToString(v);
    return unitsOf(str.asString<StringHeader>());
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

// ECMA-262 WhiteSpace + LineTerminator, which is what trim strips — a
// wider set than isspace(), and one that must not depend on a locale
// (docs/0001 decision 10).
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
        // A RangeError in the language. bronze has no exceptions (docs/0001
        // phase 4 has not reached try/catch), so it is a hard error naming
        // itself rather than a silently clamped count.
        fatal("String.prototype.repeat with a negative or infinite count");
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

// ASCII-only, and loud about it. Full case mapping is a Unicode table
// bronze does not carry, and "é".toUpperCase() answering "é" would be a
// wrong answer given quietly — the one thing the house rules forbid above
// all. When the tables land, this check is what gets deleted.
template <bool Upper>
uint64_t stringCaseImpl(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), Upper ? "toUpperCase" : "toLowerCase");
    for (uint16_t u : self) {
        if (u >= 0x80) {
            fatal(Upper ? "unsupported: String.prototype.toUpperCase on a non-ASCII string "
                          "(no Unicode case tables)"
                        : "unsupported: String.prototype.toLowerCase on a non-ASCII string "
                          "(no Unicode case tables)");
        }
    }
    Units out;
    out.reserve(self.size());
    for (uint16_t u : self) {
        if constexpr (Upper) {
            out.push_back(u >= 'a' && u <= 'z' ? static_cast<uint16_t>(u - 32) : u);
        } else {
            out.push_back(u >= 'A' && u <= 'Z' ? static_cast<uint16_t>(u + 32) : u);
        }
    }
    return stringFromUnits(out).rawBits();
}

// `$` in a replacement is a substitution pattern ($&, $1, $<name>, $$).
// Implementing the search half without it would silently drop the pattern,
// so it is diagnosed until the whole of it is written.
void rejectDollarPatterns(const Units& replacement, const char* method) {
    for (uint16_t u : replacement) {
        if (u == '$') {
            fatal((std::string("unsupported: $-substitution in String.prototype.") + method +
                   " is not implemented")
                      .c_str());
        }
    }
}

template <bool All>
uint64_t stringReplaceImpl(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const char* method = All ? "replaceAll" : "replace";
    Units self = thisUnits(Value(thisBits), method);
    if (args[0].isObject()) {
        // A RegExp or a replacer function. bronze has neither, and matching
        // a stringified object would be nonsense rather than an
        // approximation.
        fatal((std::string("unsupported: String.prototype.") + method +
               " with a non-string pattern is not implemented")
                  .c_str());
    }
    if (args[1].isObject()) {
        fatal((std::string("unsupported: String.prototype.") + method +
               " with a function replacement is not implemented")
                  .c_str());
    }
    Units needle = argUnits(args[0]);
    Units repl = argUnits(args[1]);
    rejectDollarPatterns(repl, method);

    Units out;
    size_t at = 0;
    while (at <= self.size()) {
        int64_t found = indexOfUnits(self, needle, at);
        if (found < 0) break;
        out.insert(out.end(), self.begin() + at, self.begin() + found);
        out.insert(out.end(), repl.begin(), repl.end());
        at = static_cast<size_t>(found) + needle.size();
        if constexpr (All) {
            // An empty needle matches everywhere; without this the loop
            // would never advance past the first position.
            if (needle.empty()) {
                if (at < self.size()) out.push_back(self[at]);
                ++at;
            }
        } else {
            break;
        }
    }
    if (at < self.size()) out.insert(out.end(), self.begin() + at, self.end());
    return stringFromUnits(out).rawBits();
}

uint64_t stringSplit(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Units self = thisUnits(Value(thisBits), "split");
    if (args[0].isObject()) {
        fatal("unsupported: String.prototype.split with a non-string separator is not implemented");
    }

    ArrayHeader* raw = ArrayHeader::create(rtHeap(), 4);
    raw->header.flags = 1;
    raw->length = 0;
    Rooted<Value> out{Value::fromObject(raw)};

    auto pushPiece = [&](const Units& piece) {
        Rooted<Value> val{stringFromUnits(piece)};
        uint32_t at = out.get().asObject<ArrayHeader>()->length;
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at, val);
    };

    // No separator at all: the whole string, as one element. Distinct from
    // an empty separator, which splits into single code units.
    if (args[0].isUndefined()) {
        pushPiece(self);
        return out.get().rawBits();
    }
    Units sep = argUnits(args[0]);
    if (sep.empty()) {
        for (uint16_t u : self) pushPiece(Units{u});
        return out.get().rawBits();
    }

    size_t at = 0;
    for (;;) {
        int64_t found = indexOfUnits(self, sep, at);
        if (found < 0) break;
        pushPiece(Units(self.begin() + at, self.begin() + found));
        at = static_cast<size_t>(found) + sep.size();
    }
    pushPiece(Units(self.begin() + at, self.end()));
    return out.get().rawBits();
}

uint64_t stringItself(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!self.isString()) {
        fatal("String.prototype.toString called on a value that is not a string");
    }
    return self.rawBits();
}

struct StringMethod {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const StringMethod kStringMethods[] = {
    {"at", stringAt, 1},
    {"charAt", stringCharAt, 1},
    {"concat", stringConcat, 0},
    {"endsWith", stringEndsWith, 1},
    {"includes", stringIncludes, 1},
    {"indexOf", stringIndexOf, 1},
    {"lastIndexOf", stringLastIndexOf, 1},
    {"padEnd", stringPadImpl<false>, 1},
    {"padStart", stringPadImpl<true>, 1},
    {"repeat", stringRepeat, 1},
    {"replace", stringReplaceImpl<false>, 2},
    {"replaceAll", stringReplaceImpl<true>, 2},
    {"slice", stringSlice, 0},
    {"split", stringSplit, 0},
    {"startsWith", stringStartsWith, 1},
    {"substring", stringSubstring, 0},
    {"toLowerCase", stringCaseImpl<false>, 0},
    {"toString", stringItself, 0},
    {"toUpperCase", stringCaseImpl<true>, 0},
    {"trim", stringTrimImpl<true, true>, 0},
    {"trimEnd", stringTrimImpl<false, true>, 0},
    {"trimStart", stringTrimImpl<true, false>, 0},
    {"valueOf", stringItself, 0},
};

}  // namespace

Value rtStringMethod(const std::string& key) {
    for (const StringMethod& m : kStringMethods) {
        if (key == m.name) return Value(bronze_function_singleton(m.code, m.arity));
    }
    return Value::fromUndefined();
}

}  // namespace bronze::runtime
