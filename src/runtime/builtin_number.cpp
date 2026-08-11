// The `Number` namespace (docs/0021 decision 7).
//
// Statics only. `Number.prototype.toFixed` and the rest of the wrapper
// methods are NOT here: reaching them means a primitive number answering a
// property read, which bronze has no wrapper object for, and 21.1.3.3's
// rounding is defined on the decimal expansion rather than on the double —
// getting it nearly right is the shape of silent wrong answer this project
// exists to avoid. `cases/blocked/number_methods.js` holds it.

#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
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
    int radix = 0;
    if (!radixVal.isUndefined()) {
        const double r = rtToNumber(radixVal);
        radix = std::isnan(r) ? 0 : static_cast<int>(r);
    }
    return Value::fromDouble(parseIntText(text, radix)).rawBits();
}

struct NamespaceFn {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

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

// `prototype` is on this list for the reason it is on Map's: bronze has no
// Number wrapper object, so answering `undefined` would let a program install
// a method nothing would find.
const char* const kNumberUnimplemented[] = {
    "prototype",
};

Value g_numberNamespace = Value::fromUndefined();

}  // namespace

Value rtNumberNamespace() {
    if (g_numberNamespace.isObject()) return g_numberNamespace;
    Rooted<Value> obj{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(Value::fromUndefined())))};
    obj.get().asObject<ObjectHeader>()->header.flags = 0;
    for (const NamespaceFn& fn : kNumberFunctions) {
        Rooted<Value> key{rtMakeString(fn.name)};
        Rooted<Value> val{Value(bronze_function_singleton(fn.code, fn.arity))};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }
    for (const NamespaceConst& c : kNumberConstants) {
        Rooted<Value> key{rtMakeString(c.name)};
        Rooted<Value> val{Value::fromDouble(c.value)};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }
    g_numberNamespace = obj.get();
    rtHeap().add_permanent_root(&g_numberNamespace);
    return g_numberNamespace;
}

void rtNumberCheckMissingMember(Value obj, const std::string& key) {
    if (!g_numberNamespace.isObject() || obj.rawBits() != g_numberNamespace.rawBits()) return;
    rtCheckUnimplementedMember("Number", kNumberUnimplemented, std::size(kNumberUnimplemented),
                               key);
}

}  // namespace bronze::runtime
