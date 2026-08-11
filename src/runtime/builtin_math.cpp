// The `Math` namespace: one object, built once, reachable because lowering
// resolves the free identifier `Math` to bronze_global_get rather than
// diagnosing it (docs/0011 decision 1).
//
// Every function here is an ordinary bronze function object over a native
// code pointer, so `Math.min` can be read as a value, passed around and
// called through the dynamic convention like any other. Where inference
// proves the arguments are numbers, lowering does NOT come through here at
// all — it emits the arithmetic inline (docs/0011 decision 2) — so the two
// paths have to agree on every edge case, and the oracle case runs both.

#include <cmath>
#include <limits>
#include <string>

#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

double argAt(uint32_t argc, const uint64_t* argv, uint32_t i) {
    if (i >= argc) return std::numeric_limits<double>::quiet_NaN();
    return rtToNumber(Value(argv[i]));
}

// JS Math.min/Math.max are not std::min/std::max: NaN anywhere wins, and
// -0 is less than +0 (`Math.max(-0, 0)` is `0`, `Math.min(0, -0)` is `-0`),
// which a plain `<` cannot see. Both are also variadic with an identity of
// +/-Infinity for the empty call.
bool lessForMinMax(double a, double b) {
    if (a < b) return true;
    if (a > b) return false;
    // Equal, or both zero: distinguish -0 from +0 by the sign bit.
    return std::signbit(a) && !std::signbit(b);
}

uint64_t mathMin(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    double best = std::numeric_limits<double>::infinity();
    for (uint32_t i = 0; i < argc; ++i) {
        double v = rtToNumber(Value(argv[i]));
        if (std::isnan(v)) return Value::fromDouble(v).rawBits();
        if (lessForMinMax(v, best)) best = v;
    }
    return Value::fromDouble(best).rawBits();
}

uint64_t mathMax(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    double best = -std::numeric_limits<double>::infinity();
    for (uint32_t i = 0; i < argc; ++i) {
        double v = rtToNumber(Value(argv[i]));
        if (std::isnan(v)) return Value::fromDouble(v).rawBits();
        if (lessForMinMax(best, v)) best = v;
    }
    return Value::fromDouble(best).rawBits();
}

uint64_t mathHypot(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    // std::hypot's two- and three-argument forms are the ones that avoid
    // intermediate overflow; past that, fall back to the naive sum, which
    // is what the spec's "implementation-approximated" licence allows.
    if (argc == 0) return Value::fromDouble(0.0).rawBits();
    if (argc == 1) return Value::fromDouble(std::abs(argAt(argc, argv, 0))).rawBits();
    if (argc == 2) {
        return Value::fromDouble(std::hypot(argAt(argc, argv, 0), argAt(argc, argv, 1))).rawBits();
    }
    if (argc == 3) {
        return Value::fromDouble(
                   std::hypot(argAt(argc, argv, 0), argAt(argc, argv, 1), argAt(argc, argv, 2)))
            .rawBits();
    }
    double sum = 0.0;
    for (uint32_t i = 0; i < argc; ++i) {
        double v = rtToNumber(Value(argv[i]));
        sum += v * v;
    }
    return Value::fromDouble(std::sqrt(sum)).rawBits();
}

// JS rounds half UP (towards +Infinity), so Math.round(-2.5) is -2, while
// std::round rounds half away from zero and answers -3. The whole reason
// this is not one intrinsic in generated code either (docs/0011).
double jsRound(double x) {
    if (std::isnan(x) || std::isinf(x) || x == 0.0) return x;
    double f = std::floor(x);
    if (x - f >= 0.5) f += 1.0;
    // -0.4 rounds to -0, not 0: the sign has to survive.
    if (f == 0.0 && std::signbit(x)) return -0.0;
    return f;
}

double jsSign(double x) {
    if (std::isnan(x) || x == 0.0) return x;  // preserves -0 and NaN
    return x < 0 ? -1.0 : 1.0;
}

// The unary members, each `double -> double`, so one native trampoline per
// entry is all it takes.
using UnaryFn = double (*)(double);

template <UnaryFn F>
uint64_t mathUnary(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return Value::fromDouble(F(argAt(argc, argv, 0))).rawBits();
}

double unaryAbs(double x) { return std::abs(x); }
double unaryFloor(double x) { return std::floor(x); }
double unaryCeil(double x) { return std::ceil(x); }
double unaryTrunc(double x) { return std::trunc(x); }
double unarySqrt(double x) { return std::sqrt(x); }
double unaryCbrt(double x) { return std::cbrt(x); }
double unaryExp(double x) { return std::exp(x); }
double unaryLog(double x) { return std::log(x); }
double unaryLog2(double x) { return std::log2(x); }
double unaryLog10(double x) { return std::log10(x); }
double unarySin(double x) { return std::sin(x); }
double unaryCos(double x) { return std::cos(x); }
double unaryTan(double x) { return std::tan(x); }
double unaryAsin(double x) { return std::asin(x); }
double unaryAcos(double x) { return std::acos(x); }
double unaryAtan(double x) { return std::atan(x); }

uint64_t mathPow(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    // `Math.pow(a, b)` and `a ** b` are the SAME operation in ECMA-262
    // (Number::exponentiate), so they are the same function here. Two
    // spellings of the NaN rules are two places for them to drift.
    return Value::fromDouble(
               rtExponentiate(argAt(argc, argv, 0), argAt(argc, argv, 1)))
        .rawBits();
}

uint64_t mathAtan2(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return Value::fromDouble(std::atan2(argAt(argc, argv, 0), argAt(argc, argv, 1))).rawBits();
}

struct MathFn {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

struct MathConst {
    const char* name;
    double value;
};

const MathFn kMathFunctions[] = {
    {"abs", mathUnary<unaryAbs>, 1},     {"floor", mathUnary<unaryFloor>, 1},
    {"ceil", mathUnary<unaryCeil>, 1},   {"trunc", mathUnary<unaryTrunc>, 1},
    {"round", mathUnary<jsRound>, 1},    {"sign", mathUnary<jsSign>, 1},
    {"sqrt", mathUnary<unarySqrt>, 1},   {"cbrt", mathUnary<unaryCbrt>, 1},
    {"exp", mathUnary<unaryExp>, 1},     {"log", mathUnary<unaryLog>, 1},
    {"log2", mathUnary<unaryLog2>, 1},   {"log10", mathUnary<unaryLog10>, 1},
    {"sin", mathUnary<unarySin>, 1},     {"cos", mathUnary<unaryCos>, 1},
    {"tan", mathUnary<unaryTan>, 1},     {"asin", mathUnary<unaryAsin>, 1},
    {"acos", mathUnary<unaryAcos>, 1},   {"atan", mathUnary<unaryAtan>, 1},
    {"atan2", mathAtan2, 2},             {"pow", mathPow, 2},
    // Arity 0 is not "takes nothing": FunctionHeader::arity is the count a
    // short call is PADDED with undefined up to, and a variadic builtin
    // must see the real argc — `Math.min()` is Infinity, while the same
    // call padded to two undefineds is NaN. The three below are the
    // variadic ones; the rest declare their real parameter count so a short
    // call reaches them as the language says, with undefined.
    {"min", mathMin, 0},                 {"max", mathMax, 0},
    {"hypot", mathHypot, 0},
};

const MathConst kMathConstants[] = {
    {"E", 2.718281828459045},       {"LN10", 2.302585092994046},
    {"LN2", 0.6931471805599453},    {"LOG10E", 0.4342944819032518},
    {"LOG2E", 1.4426950408889634},  {"PI", 3.141592653589793},
    {"SQRT1_2", 0.7071067811865476}, {"SQRT2", 1.4142135623730951},
};

// Real members of the Math namespace that bronze has NOT built. Reading one
// must not be `undefined`: a program that feature-tests `Math.random` and
// finds it missing takes a branch no JS engine would take. Same rule as the
// prototype tables in rt_helpers.cpp — membership here is ECMA-262's "does
// this exist?", never "have we got round to it?".
//
// `random` is on this list on purpose and not for want of a call to a PRNG:
// bronze has no decision about a seed, and a nondeterministic global would
// need one before it can be defended (docs/0001 decision 10).
const char* const kMathUnimplemented[] = {
    "acosh", "asinh", "atanh", "clz32", "cosh", "expm1", "f16round", "fround",
    "imul",  "log1p", "random", "sinh",  "sumPrecise", "tanh",
};

Value g_mathObject = Value::fromUndefined();

}  // namespace

Value rtMathObject() {
    if (g_mathObject.isObject()) return g_mathObject;

    // Its own root shape, not the one every `{}` literal shares: a property
    // site reading `Math.sqrt` and one reading `point.x` would otherwise
    // walk the same transition tree and miss each other's caches forever.
    Rooted<Value> obj{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(Value::fromUndefined())))};
    obj.get().asObject<ObjectHeader>()->header.flags = 0;

    for (const MathFn& fn : kMathFunctions) {
        Rooted<Value> key{rtMakeString(fn.name)};
        Rooted<Value> val{Value(bronze_function_singleton(fn.code, fn.arity))};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }
    for (const MathConst& c : kMathConstants) {
        Rooted<Value> key{rtMakeString(c.name)};
        Rooted<Value> val{Value::fromDouble(c.value)};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }

    g_mathObject = obj.get();
    rtHeap().add_permanent_root(&g_mathObject);
    return g_mathObject;
}

void rtMathCheckMissingMember(Value obj, const std::string& key) {
    if (!g_mathObject.isObject() || obj.rawBits() != g_mathObject.rawBits()) return;
    rtCheckUnimplementedMember("Math", kMathUnimplemented, std::size(kMathUnimplemented), key);
}

}  // namespace bronze::runtime
