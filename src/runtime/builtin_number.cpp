// The `Number` CONSTRUCTOR object: what the identifier `Number` denotes, its
// conversion body (21.1.1.1), and the statics of 21.1.2.
//
// It is a function object and not a namespace object, which it was for as long
// as bronze had no `Number.prototype` for `new Number(1)` to build an instance
// of. Both halves of 21.1.1.1 follow from that one change: `Number("5")` is a
// CALL, where a namespace object could only report that an object is not
// callable, and `new Number(5)` is a Number exotic object with a [[NumberData]]
// slot, which `bronze_construct` builds from the ctor table rather than by
// entering this body (builtin_wrappers.cpp says why a native constructor cannot
// see NewTarget).
//
// `Number.prototype.toFixed` and the rest of the wrapper methods are NOT here:
// they are members of the intrinsic prototype object, installed on it by
// builtin_number_proto.cpp. The split ECMA-262 itself draws — 21.1.2 is the
// constructor's own properties, 21.1.3 is the prototype's — is the split
// between the two files.

#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/bigint.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// 21.1.2.2 / 21.1.2.3 / 21.1.2.5 are deliberately NOT ToNumber-coercing: a
// string argument answers false, which is the whole difference between
// `Number.isNaN("x")` (false) and the global `isNaN("x")` (true).
uint64_t numberIsNaN(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const Value v = args[0];
    return Value::fromBool(v.isNumber() && std::isnan(v.asNumber())).rawBits();
}

uint64_t numberIsFinite(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const Value v = args[0];
    return Value::fromBool(v.isNumber() && std::isfinite(v.asNumber())).rawBits();
}

uint64_t numberIsInteger(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const Value v = args[0];
    return Value::fromBool(v.isNumber() && std::isfinite(v.asNumber()) &&
                           std::trunc(v.asNumber()) == v.asNumber())
        .rawBits();
}

uint64_t numberIsSafeInteger(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const Value v = args[0];
    const bool integral = v.isNumber() && std::isfinite(v.asNumber()) &&
                          std::trunc(v.asNumber()) == v.asNumber();
    return Value::fromBool(integral && std::abs(v.asNumber()) <= 9007199254740991.0).rawBits();
}

bool isTrimmable(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// ECMA-262 19.2.4 parseFloat: the LONGEST PREFIX that is a StrDecimalLiteral,
// which is what makes it different from ToNumber — `parseFloat("3.5px")` is
// 3.5 where `Number("3.5px")` is NaN.
double parseFloatText(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && isTrimmable(s[i])) ++i;
    size_t start = i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    if (s.compare(i, 8, "Infinity") == 0) {
        return s[start] == '-' ? -std::numeric_limits<double>::infinity()
                               : std::numeric_limits<double>::infinity();
    }
    size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++digits; }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++digits; }
    }
    if (digits == 0) return std::numeric_limits<double>::quiet_NaN();
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        size_t save = i;
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        size_t expDigits = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++expDigits; }
        if (expDigits == 0) i = save;  // a bare `e` is not part of the literal
    }
    return std::strtod(s.substr(start, i - start).c_str(), nullptr);
}

// 19.2.5 parseInt, including the radix argument and the `0x` prefix that
// selects radix 16 when no radix was given.
double parseIntText(const std::string& s, int radix) {
    size_t i = 0;
    while (i < s.size() && isTrimmable(s[i])) ++i;
    int sign = 1;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        if (s[i] == '-') sign = -1;
        ++i;
    }
    if ((radix == 0 || radix == 16) && i + 1 < s.size() && s[i] == '0' &&
        (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        i += 2;
        radix = 16;
    }
    if (radix == 0) radix = 10;
    if (radix < 2 || radix > 36) return std::numeric_limits<double>::quiet_NaN();

    double value = 0.0;
    size_t digits = 0;
    for (; i < s.size(); ++i) {
        int d;
        const char c = s[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= radix) break;
        value = value * radix + d;
        ++digits;
    }
    if (digits == 0) return std::numeric_limits<double>::quiet_NaN();
    return sign * value;
}

std::string textOf(Value v) {
    Rooted<Value> str{rtValueToString(v)};
    return rtAsciiChars(str.get().asString<StringHeader>());
}

uint64_t numberParseFloat(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return Value::fromDouble(parseFloatText(textOf(args[0]))).rawBits();
}

uint64_t numberParseInt(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const std::string text = textOf(args[0]);
    const Value radixVal = args[1];
    // 19.2.5 step 4 is ToInt32(radix), not a truncation: `parseInt("10",
    // Infinity)` is 10 because ToInt32(Infinity) is 0, which selects the
    // default radix. A `static_cast<int>` of an infinity or a NaN is undefined
    // behaviour, and on this target it produced INT_MIN — so the radix range
    // check rejected it and the answer was NaN.
    int radix = 0;
    if (!radixVal.isUndefined()) {
        radix = bronze_to_int32_f64(rtToNumber(radixVal));
    }
    return Value::fromDouble(parseIntText(text, radix)).rawBits();
}

// ECMA-262 19.2.2 / 19.2.3: the GLOBAL predicates, which DO coerce. They are
// separate functions from `Number.isNaN` / `Number.isFinite` above, and the
// difference is the whole reason both exist — `isNaN("x")` is true because
// ToNumber("x") is NaN, and `Number.isNaN("x")` is false because a string is
// not a Number.
uint64_t globalIsNaN(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return Value::fromBool(std::isnan(rtToNumber(args[0]))).rawBits();
}

uint64_t globalIsFinite(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return Value::fromBool(std::isfinite(rtToNumber(args[0]))).rawBits();
}

struct NamespaceFn {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

// The two tables below are the whole of 21.1.2 but for `prototype`, which the
// ctor table answers from the intrinsic — which is why this file has no
// unimplemented-member list at all. `Boolean` has none for the same reason, and
// its row in `kCtors` carries a null one.
const NamespaceFn kNumberFunctions[] = {
    {"isNaN", numberIsNaN, 1},           {"isFinite", numberIsFinite, 1},
    {"isInteger", numberIsInteger, 1},   {"isSafeInteger", numberIsSafeInteger, 1},
    {"parseFloat", numberParseFloat, 1}, {"parseInt", numberParseInt, 2},
};

struct NamespaceConst {
    const char* name;
    double value;
};

const NamespaceConst kNumberConstants[] = {
    {"MAX_SAFE_INTEGER", 9007199254740991.0},
    {"MIN_SAFE_INTEGER", -9007199254740991.0},
    {"MAX_VALUE", 1.7976931348623157e308},
    {"MIN_VALUE", 5e-324},
    {"EPSILON", 2.220446049250313e-16},
    {"POSITIVE_INFINITY", std::numeric_limits<double>::infinity()},
    {"NEGATIVE_INFINITY", -std::numeric_limits<double>::infinity()},
    {"NaN", std::numeric_limits<double>::quiet_NaN()},
};

static bool isUriUnescaped(unsigned char c, bool component) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    switch (c) {
        case '-': case '_': case '.': case '!': case '~': case '*': case '\'': case '(': case ')':
            return true;
        case ';': case ',': case '/': case '?': case ':': case '@': case '&': case '=': case '+': case '$': case '#':
            return !component;
        default:
            return false;
    }
}

static std::string encodeUriImpl(const std::string& input, bool component) {
    static const char hexChars[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(input.size() * 3);
    for (unsigned char c : input) {
        if (isUriUnescaped(c, component)) {
            result.push_back(c);
        } else {
            result.push_back('%');
            result.push_back(hexChars[(c >> 4) & 0xF]);
            result.push_back(hexChars[c & 0xF]);
        }
    }
    return result;
}

uint64_t globalEncodeURI(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    std::string s = args.count() > 0 ? textOf(args[0]) : "undefined";
    return rtMakeString(encodeUriImpl(s, /*component=*/false)).rawBits();
}

uint64_t globalEncodeURIComponent(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    std::string s = args.count() > 0 ? textOf(args[0]) : "undefined";
    return rtMakeString(encodeUriImpl(s, /*component=*/true)).rawBits();
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 19.2.6.2 Decode. The two entry points differ in ONE set: `decodeURI` keeps
// a reserved character's escape spelled as it was written — decoding `%2F`
// would change where a path splits — while `decodeURIComponent` decodes
// everything. A truncated or non-hex escape is the spec's URIError in both
// (19.2.6.1.1), not a byte passed through: passing it through was a silent
// wrong answer with the same shape as the reserved set being decoded.
static bool decodeUriImpl(const std::string& input, bool preserveReserved,
                          std::string& result) {
    result.reserve(input.size());
    const size_t len = input.size();
    for (size_t i = 0; i < len; ++i) {
        if (input[i] != '%') {
            result.push_back(input[i]);
            continue;
        }
        if (i + 2 >= len) return false;
        const int h1 = hexVal(input[i + 1]);
        const int h2 = hexVal(input[i + 2]);
        if (h1 < 0 || h2 < 0) return false;
        const char decoded = static_cast<char>((h1 << 4) | h2);
        if (preserveReserved && !isUriUnescaped(static_cast<unsigned char>(decoded),
                                                /*component=*/true) &&
            isUriUnescaped(static_cast<unsigned char>(decoded), /*component=*/false)) {
            // In the reserved set (uriReserved + '#'): the difference between
            // the two isUriUnescaped answers IS that set.
            result.append(input, i, 3);
        } else {
            result.push_back(decoded);
        }
        i += 2;
    }
    return true;
}

uint64_t globalDecodeURI(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    std::string s = args.count() > 0 ? textOf(args[0]) : "undefined";
    std::string decoded;
    if (!decodeUriImpl(s, /*preserveReserved=*/true, decoded)) {
        return rtThrowError(ErrorKind::URIError, "URI malformed").rawBits();
    }
    return rtMakeString(decoded).rawBits();
}

uint64_t globalDecodeURIComponent(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    std::string s = args.count() > 0 ? textOf(args[0]) : "undefined";
    std::string decoded;
    if (!decodeUriImpl(s, /*preserveReserved=*/false, decoded)) {
        return rtThrowError(ErrorKind::URIError, "URI malformed").rawBits();
    }
    return rtMakeString(decoded).rawBits();
}

// ---- Annex B B.2.1: escape / unescape ---------------------------------------
//
// They live beside the URI pair above because they answer the same question one
// generation earlier — "how do I put arbitrary text where only a restricted
// alphabet is allowed" — and because the table below is the one list of what
// the GLOBAL OBJECT carries as a function. Annex B is normative for a web
// browser, and bronze's target is browser code (three.js and pixi.js are the
// pinned milestones), so these are not optional there.
//
// Both work in CODE UNITS, not UTF-8 bytes: `escape` of a character above 0xFF
// is one `%uXXXX` per unit — so an astral character becomes TWO of them, its
// surrogates escaped separately — and `unescape` reverses exactly that. A
// byte-wise implementation would round-trip ASCII and silently mangle the rest.

// The 69 characters B.2.1.1 step 4 keeps as themselves. Everything else is
// escaped, which is why the set is written out rather than derived: `+` is here
// and `~` is not, and no rule of thumb gets that right.
bool isEscapeUnescaped(uint16_t unit) {
    if (unit >= 0x80) return false;
    const char c = static_cast<char>(unit);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
    return c == '@' || c == '*' || c == '_' || c == '+' || c == '-' || c == '.' || c == '/';
}

void appendUpperHex(std::string& out, uint32_t value, size_t width) {
    static const char kDigits[] = "0123456789ABCDEF";
    for (size_t shift = width * 4; shift > 0; shift -= 4) {
        out.push_back(kDigits[(value >> (shift - 4)) & 0xF]);
    }
}

// B.2.1.1 escape(string).
uint64_t globalEscape(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // Step 1 is ToString, and an absent argument is `undefined` — so
    // `escape()` is "undefined", the same answer the URI functions above give.
    Rooted<Value> input{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    const std::vector<uint16_t> units = rtStringUnits(input.get().asString<StringHeader>());
    // The result is ASCII by construction, so a UTF-8 std::string carries it
    // exactly and `rtMakeString` needs no unit array.
    std::string out;
    for (const uint16_t unit : units) {
        if (isEscapeUnescaped(unit)) {
            out.push_back(static_cast<char>(unit));
        } else if (unit < 0x100) {
            out.push_back('%');
            appendUpperHex(out, unit, 2);
        } else {
            out += "%u";
            appendUpperHex(out, unit, 4);
        }
    }
    return rtMakeString(out).rawBits();
}

// B.2.1.2 unescape(string). A `%` that does not begin a well-formed escape is
// KEPT as a `%` (steps 5.b and 5.c fall through), which is what makes
// `unescape("%")` and `unescape("%zz")` answer themselves rather than throw —
// the one place this pair differs in spirit from `decodeURI`, whose malformed
// input is a URIError.
uint64_t globalUnescape(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> input{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    const std::vector<uint16_t> units = rtStringUnits(input.get().asString<StringHeader>());
    std::vector<uint16_t> out;
    out.reserve(units.size());
    const size_t len = units.size();
    for (size_t i = 0; i < len; ++i) {
        if (units[i] != '%') {
            out.push_back(units[i]);
            continue;
        }
        // `%uXXXX` first: it is the longer form, and `%u0041` must not be read
        // as `%u0` followed by "041".
        //
        // The `u` is lowercase only — step 5.b spells the code unit 0x0075 —
        // so `%U0041` is not an escape and stays six characters.
        if (i + 5 < len && units[i + 1] == 'u') {
            int value = 0;
            bool ok = true;
            for (size_t k = 0; k < 4 && ok; ++k) {
                const uint16_t unit = units[i + 2 + k];
                const int digit = unit < 0x80 ? hexVal(static_cast<char>(unit)) : -1;
                if (digit < 0) ok = false;
                else value = value * 16 + digit;
            }
            if (ok) {
                out.push_back(static_cast<uint16_t>(value));
                i += 5;
                continue;
            }
        }
        if (i + 2 < len) {
            const int h1 = units[i + 1] < 0x80 ? hexVal(static_cast<char>(units[i + 1])) : -1;
            const int h2 = units[i + 2] < 0x80 ? hexVal(static_cast<char>(units[i + 2])) : -1;
            if (h1 >= 0 && h2 >= 0) {
                out.push_back(static_cast<uint16_t>((h1 << 4) | h2));
                i += 2;
                continue;
            }
        }
        out.push_back('%');
    }
    return rtStringFromUnits(out).rawBits();
}

// 19.2's function properties of the global object, by the name a free
// identifier spells. `parseInt` and `parseFloat` share their code pointers with
// the `Number` statics, and `bronze_function_singleton` interns by code
// pointer, so the two names denote ONE object — 21.1.2.12 and 21.1.2.13 say
// exactly that ("the same function object").
const NamespaceFn kGlobalFunctions[] = {
    {"isNaN", globalIsNaN, 1},
    {"isFinite", globalIsFinite, 1},
    {"parseInt", numberParseInt, 2},
    {"parseFloat", numberParseFloat, 1},
    {"encodeURI", globalEncodeURI, 1},
    {"encodeURIComponent", globalEncodeURIComponent, 1},
    {"decodeURI", globalDecodeURI, 1},
    {"decodeURIComponent", globalDecodeURIComponent, 1},
    {"escape", globalEscape, 1},
    {"unescape", globalUnescape, 1},
};

// Whether this function's statics have been installed. A plain bool and not a
// look-before-you-write, because `rtNativeFunction` interns on the code pointer:
// every route to `Number` reaches the SAME function object, so installing twice
// would be redefining the same fourteen properties rather than decorating two
// objects.
thread_local bool g_numberStaticsInstalled = false;

}  // namespace

Value rtGlobalNumericFunction(const std::string& name) {
    for (const NamespaceFn& fn : kGlobalFunctions) {
        if (name == fn.name) return rtNativeFunction(fn.code, fn.arity);
    }
    return Value::fromUndefined();
}

// 21.1.1.1 as a CONVERSION, which is what the body is when NewTarget is absent.
// The `new` form never enters here — `bronze_construct` recognises this
// function object and builds the Number exotic object from the same step 1.
//
// Named at namespace scope rather than through an accessor because
// `builtin_constructors.cpp`'s ctor table takes its ADDRESS at static
// initialization, and a function that returned the pointer would make that a
// cross-translation-unit initialization order.
uint64_t rtNumberConstructorBody(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // Step 2: no argument AT ALL is +0𝔽, which is not the same as
    // `Number(undefined)` — that one is ToNumber(undefined), NaN.
    if (args.count() == 0) return Value::fromDouble(0.0).rawBits();
    return rtNumberValueOfArgument(args[0]).rawBits();
}

Value rtNumberValueOfArgument(Value v) {
    // 21.1.1.1 step 1 is ToNumeric, whose step 1 is ToPrimitive with hint
    // NUMBER — so an object is asked for `valueOf` before `toString`, and a
    // program's override runs. Spelled out here rather than left to
    // `rtToNumber`, which performs the same step, because the SYMBOL case below
    // has to name `Number` in its message: 21.1.1.1 refuses one before any
    // conversion is attempted.
    Rooted<Value> input{v};
    Rooted<Value> prim{rtToPrimitive(input, ToPrimitiveHint::Number)};
    if (rtExceptionPending()) return Value::fromDouble(0.0);
    // 6.1.5.1: ToNumber of a Symbol is a TypeError, and one a `catch` can
    // hold — the hook the conversion is asked through does not change that.
    if (prim.get().isSymbol()) {
        return rtThrowTypeError("Cannot convert a Symbol value to a number");
    }
    // Step 2: a BigInt CONVERTS here, where `rtToNumber` refuses it. That is
    // the whole difference between the explicit conversion and the implicit
    // one — `Number(1n)` is 1 and `1n * 1` is a TypeError — and it is why this
    // clause is spelled out instead of delegating. The rounding is 6.1.6.2's
    // ℝ -> Number, so a value past 2^53 lands on the nearest double.
    if (prim.get().isBigInt()) return Value::fromDouble(rtBigIntToNumber(prim.get()));
    return Value::fromDouble(rtToNumber(prim.get()));
}

void rtInstallNumberStatics(Rooted<Value>& fn) {
    if (g_numberStaticsInstalled) return;
    g_numberStaticsInstalled = true;
    rtEnsureFunctionProperties(fn);
    Rooted<Value> props{fn.get().asObject<FunctionHeader>()->properties};
    // NON-ENUMERABLE, every one of them, because that is the attribute 21.1.2
    // gives all fifteen and it is the one this storage can express: a shape
    // transition carries `enumerable`, where `writable` and `configurable` are
    // fixed true by the transition tree and false only in dictionary mode. So
    // `Object.keys(Number)` is `[]` and `for (k in Number)` visits nothing,
    // which is what a program can see; the other two attributes stay a
    // divergence this object shares with every intrinsic in bronze
    // (cases/blocked/intrinsic_property_attributes).
    for (const NamespaceFn& f : kNumberFunctions) {
        Rooted<Value> key{rtMakeString(f.name)};
        Rooted<Value> val{rtNativeFunction(f.code, f.arity)};
        props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
    for (const NamespaceConst& c : kNumberConstants) {
        Rooted<Value> key{rtMakeString(c.name)};
        Rooted<Value> val{Value::fromDouble(c.value)};
        props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
}

}  // namespace bronze::runtime
