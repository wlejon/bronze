// The `Math` namespace: one object, built once, reachable because lowering
// resolves the free identifier `Math` to bronze_global_get rather than
// diagnosing it.
//
// Every function here is an ordinary bronze function object over a native code
// pointer, so `Math.min` can be read as a value, passed around and called
// through the dynamic convention like any other. Where inference proves the
// arguments are numbers, lowering does NOT come through here at all — it emits
// the arithmetic inline — so the two paths have to agree on every edge case,
// and the oracle case runs both.

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
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
// std::round rounds half away from zero and answers -3. The whole reason this
// is not one intrinsic in generated code either.
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

double unaryFloor(double x) { return std::floor(x); }
double unaryCeil(double x) { return std::ceil(x); }
double unaryTrunc(double x) { return std::trunc(x); }
double unaryCbrt(double x) { return std::cbrt(x); }
double unaryExp(double x) { return std::exp(x); }
double unaryExpm1(double x) { return std::expm1(x); }
double unaryLog(double x) { return std::log(x); }
double unaryLog1p(double x) { return std::log1p(x); }
double unaryLog2(double x) { return std::log2(x); }
double unaryLog10(double x) { return std::log10(x); }
double unaryTan(double x) { return std::tan(x); }
double unaryAsin(double x) { return std::asin(x); }
double unaryAcos(double x) { return std::acos(x); }
double unaryAtan(double x) { return std::atan(x); }
double unarySinh(double x) { return std::sinh(x); }
double unaryCosh(double x) { return std::cosh(x); }
double unaryTanh(double x) { return std::tanh(x); }
double unaryAsinh(double x) { return std::asinh(x); }
double unaryAcosh(double x) { return std::acosh(x); }
double unaryAtanh(double x) { return std::atanh(x); }
double unaryFround(double x) { return static_cast<double>(static_cast<float>(x)); }

uint64_t mathClz32(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc == 0) return Value::fromDouble(32.0).rawBits();
    uint32_t n = static_cast<uint32_t>(bronze_to_int32(argv[0]));
    if (n == 0) return Value::fromDouble(32.0).rawBits();
    return Value::fromDouble(static_cast<double>(std::countl_zero(n))).rawBits();
}

uint64_t mathImul(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    uint32_t a = argc > 0 ? static_cast<uint32_t>(bronze_to_int32(argv[0])) : 0;
    uint32_t b = argc > 1 ? static_cast<uint32_t>(bronze_to_int32(argv[1])) : 0;
    return Value::fromDouble(static_cast<int32_t>(a * b)).rawBits();
}

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

// ---- Math.random (21.3.2.27) ------------------------------------------------
//
// The generator is xoshiro256++ (Blackman & Vigna), seeded once from the OS. A
// COMPILED PROGRAM is allowed to be nondeterministic where bronze's own output
// is not. A fixed seed would have been the silent wrong answer rather than the
// safe choice: a program that asks for randomness and is handed a replayable
// sequence has been lied to.
//
// Not `rand()`, for four reasons that compound: MSVC's `RAND_MAX` is 32767, so
// it yields fifteen bits per call and a double built from it lands on a coarse
// lattice; it is a Lehmer LCG whose low-order bits have a short period; its
// state is a single global shared with any C library code in the process, so a
// third party calling `srand` changes a JS program's sequence; and `srand`
// takes 32 bits, which is fewer distinct streams than a long-running program
// can exhaust. xoshiro256++ is four 64-bit words, one non-linear output mix, a
// period of 2^256-1, and it passes BigCrush.
struct Xoshiro256pp {
    uint64_t s[4];

    Xoshiro256pp() {
        // SplitMix64, the seeding companion the xoshiro authors specify: it
        // turns any 64-bit value into a full-entropy stream, so a source that
        // hands back correlated or mostly-zero words cannot leave the state
        // near all-zeros — the one state xoshiro escapes only slowly.
        //
        // `std::random_device` is the OS entropy source (RtlGenRandom on
        // Windows, getrandom on Linux). The clock and a stack address are
        // mixed in as well so that a platform whose `random_device` is a
        // deterministic fallback still differs from run to run rather than
        // silently repeating one sequence forever.
        std::random_device rd;
        uint64_t seed = 0x9E3779B97F4A7C15ULL;
        for (int i = 0; i < 8; ++i) {
            seed = seed * 0x100000001B3ULL + rd();
        }
        seed ^= static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        seed ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&seed));
        for (uint64_t& word : s) {
            seed += 0x9E3779B97F4A7C15ULL;
            uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            word = z ^ (z >> 31);
        }
    }

    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

    uint64_t next() {
        const uint64_t result = rotl(s[0] + s[3], 23) + s[0];
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }
};

// Constructed on first use, so a program that never calls `Math.random` never
// touches the OS entropy source.
Xoshiro256pp& prng() {
    static Xoshiro256pp g;
    return g;
}

uint64_t mathRandom(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    // 53 bits — a double's whole mantissa — over 2^53, which is every
    // representable double in [0, 1) with equal spacing and no rounding step
    // that could produce exactly 1.0. The bits taken are the HIGH ones: this
    // generator's output is uniform in every bit, but taking the top is what
    // keeps the conversion correct if the generator is ever replaced by one
    // whose low bits are weaker.
    const double x = static_cast<double>(prng().next() >> 11) / 9007199254740992.0;
    return Value::fromDouble(x).rawBits();
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
    // The six with exported code pointers (bronze_abi.h) are the ones a call
    // site can dispatch directly; the registration here is what makes the
    // guard's pointer compare mean "still the intrinsic".
    {"abs", bronze_math_abs, 1},         {"floor", mathUnary<unaryFloor>, 1},
    {"ceil", mathUnary<unaryCeil>, 1},   {"trunc", mathUnary<unaryTrunc>, 1},
    {"round", mathUnary<jsRound>, 1},    {"sign", mathUnary<jsSign>, 1},
    {"sqrt", bronze_math_sqrt, 1},       {"cbrt", mathUnary<unaryCbrt>, 1},
    {"exp", mathUnary<unaryExp>, 1},     {"expm1", mathUnary<unaryExpm1>, 1},
    {"log", mathUnary<unaryLog>, 1},     {"log1p", mathUnary<unaryLog1p>, 1},
    {"log2", mathUnary<unaryLog2>, 1},   {"log10", mathUnary<unaryLog10>, 1},
    {"sin", bronze_math_sin, 1},         {"cos", bronze_math_cos, 1},
    {"tan", mathUnary<unaryTan>, 1},     {"asin", mathUnary<unaryAsin>, 1},
    {"acos", mathUnary<unaryAcos>, 1},   {"atan", mathUnary<unaryAtan>, 1},
    {"sinh", mathUnary<unarySinh>, 1},   {"cosh", mathUnary<unaryCosh>, 1},
    {"tanh", mathUnary<unaryTanh>, 1},   {"asinh", mathUnary<unaryAsinh>, 1},
    {"acosh", mathUnary<unaryAcosh>, 1}, {"atanh", mathUnary<unaryAtanh>, 1},
    {"fround", mathUnary<unaryFround>, 1},
    {"clz32", mathClz32, 1},             {"imul", mathImul, 2},
    {"atan2", mathAtan2, 2},             {"pow", mathPow, 2},
    // Arity 0 is not "takes nothing": FunctionHeader::arity is the count a
    // short call is PADDED with undefined up to, and a variadic builtin
    // must see the real argc — `Math.min()` is Infinity, while the same
    // call padded to two undefineds is NaN. The three below are the
    // variadic ones; the rest declare their real parameter count so a short
    // call reaches them as the language says, with undefined.
    {"min", bronze_math_min, 0},         {"max", bronze_math_max, 0},
    {"hypot", mathHypot, 0},             {"random", mathRandom, 0},
};

const MathConst kMathConstants[] = {
    {"E", 2.718281828459045},       {"LN10", 2.302585092994046},
    {"LN2", 0.6931471805599453},    {"LOG10E", 0.4342944819032518},
    {"LOG2E", 1.4426950408889634},  {"PI", 3.141592653589793},
    {"SQRT1_2", 0.7071067811865476}, {"SQRT2", 1.4142135623730951},
};

// Real members of the Math namespace that bronze has NOT built. Reading one
// must not be `undefined`: a program that feature-tests a member and finds it
// missing takes a branch no JS engine would take. Same rule as the prototype
// tables in rt_helpers.cpp — membership here is ECMA-262's "does this exist?",
// never "have we got round to it?".
const char* const kMathUnimplemented[] = {
    "f16round", "sumPrecise",
};

Value g_mathObject = Value::fromUndefined();

}  // namespace

// ---- The direct-dispatch surface (bronze_abi.h) -----------------------------
//
// The scalar kernels first: each is the one C function BOTH paths run for a
// member whose result is not IEEE-pinned. Generated code's fast path calls
// them on already-unboxed doubles; the helper path reaches the same functions
// through the trampolines below, so the two paths are the same instructions
// and cannot drift by a bit. `sqrt` and `abs` have no kernel because
// llvm.sqrt.f64 and llvm.fabs.f64 are IEEE-exact — the same licence the
// inferred-arithmetic path has always used.

extern "C" double bronze_math_sin_f64(double x) { return std::sin(x); }
extern "C" double bronze_math_cos_f64(double x) { return std::cos(x); }

// Exactly ECMA-262 21.3.2.25/21.3.2.24 for two number arguments, phrased so a
// pass over {a, b} matches the variadic natives' fold: NaN anywhere wins, and
// -0 sorts below +0.
extern "C" double bronze_math_min2_f64(double a, double b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    return lessForMinMax(b, a) ? b : a;
}

extern "C" double bronze_math_max2_f64(double a, double b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    return lessForMinMax(a, b) ? b : a;
}

// The function objects' code pointers. Same bodies the anonymous statics had;
// exported names, because the call-site guard compares against the symbol.

extern "C" uint64_t bronze_math_sqrt(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return Value::fromDouble(std::sqrt(argAt(argc, argv, 0))).rawBits();
}

extern "C" uint64_t bronze_math_sin(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return Value::fromDouble(bronze_math_sin_f64(argAt(argc, argv, 0))).rawBits();
}

extern "C" uint64_t bronze_math_cos(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return Value::fromDouble(bronze_math_cos_f64(argAt(argc, argv, 0))).rawBits();
}

extern "C" uint64_t bronze_math_abs(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return Value::fromDouble(std::abs(argAt(argc, argv, 0))).rawBits();
}

extern "C" uint64_t bronze_math_min(uint64_t e, uint64_t t, uint32_t argc, const uint64_t* argv) {
    return mathMin(e, t, argc, argv);
}

extern "C" uint64_t bronze_math_max(uint64_t e, uint64_t t, uint32_t argc, const uint64_t* argv) {
    return mathMax(e, t, argc, argv);
}

Value rtMathObject() {
    if (g_mathObject.isObject()) return g_mathObject;

    // Its own root shape, not the one every `{}` literal shares: a property
    // site reading `Math.sqrt` and one reading `point.x` would otherwise
    // walk the same transition tree and miss each other's caches forever.
    Rooted<Value> obj{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(Value::fromUndefined())))};
    obj.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;

    for (const MathFn& fn : kMathFunctions) {
        Rooted<Value> key{rtMakeString(fn.name)};
        Rooted<Value> val{rtNativeFunction(fn.code, fn.arity)};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }
    for (const MathConst& c : kMathConstants) {
        Rooted<Value> key{rtMakeString(c.name)};
        Rooted<Value> val{Value::fromDouble(c.value)};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }

    // 21.3.1.9: `Math[@@toStringTag]` is the string "Math", which is what makes
    // `Object.prototype.toString.call(Math)` read "[object Math]". An own
    // property of this object in the specification, so it is one here too.
    rtDefineToStringTag(obj, "Math");

    g_mathObject = obj.get();
    rtHeap().add_permanent_root(&g_mathObject);
    return g_mathObject;
}

void rtMathCheckMissingMember(Value obj, const std::string& key) {
    if (!g_mathObject.isObject() || obj.rawBits() != g_mathObject.rawBits()) return;
    rtCheckUnimplementedMember("Math", kMathUnimplemented, std::size(kMathUnimplemented), key);
}

}  // namespace bronze::runtime
