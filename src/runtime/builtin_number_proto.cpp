// `Number.prototype` — the four formatting methods (ECMA-262 21.1.3).
//
// Split from builtin_number.cpp along the line ECMA-262 itself draws: that file
// is the `Number` NAMESPACE, whose members are statics reached through an
// object a program holds, and this one is the WRAPPER's methods, reached
// through a property read on a primitive. bronze has no Number wrapper object,
// so the property path hands these out directly the way it already does for
// `String.prototype`.
//
// All four are defined on the exact real number the double denotes, which is
// why every digit below comes from `exact_decimal.h` and none from printf or a
// to_chars round-trip: `(1.005).toFixed(2)` is "1.00" and any implementation
// that answers "1.01" is wrong in exactly the code that calls toFixed.

#include <cmath>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exact_decimal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/number_format.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// 21.1.3's thisNumberValue: bronze has no Number wrapper, so the receiver is
// the primitive or it is a TypeError. A non-number reaching here means the
// method object escaped its receiver, which a program can do.
bool thisNumber(Value self, const char* method, double& out) {
    if (!self.isNumber()) {
        rtThrowTypeError(std::string("Number.prototype.") + method +
                         " called on a value that is not a number");
        return false;
    }
    out = self.asNumber();
    return true;
}

// 7.1.5 ToIntegerOrInfinity.
double toIntegerOrInfinity(Value v) {
    const double n = rtToNumber(v);
    if (std::isnan(n)) return 0.0;
    if (!std::isfinite(n)) return n;
    const double t = std::trunc(n);
    return t == 0.0 ? 0.0 : t;
}

Value stringResult(const std::string& text) { return rtMakeString(text); }

std::string toStringOfNumber(double x) {
    char buf[40];
    const size_t len = formatJsNumber(x, buf);
    return std::string(buf, len);
}

// The digit string of the integer `n` and the exponent `e` for which
// `10^(digits-1) <= n < 10^digits` and `n × 10^(e-digits+1)` is the value
// closest to `x`, ties away from zero. That is one abstract operation used by
// both 21.1.3.2 step 10.a (with digits = f+1) and 21.1.3.5 step 10 (with
// digits = p) — the two clauses are the same search with a different width,
// and writing it twice would be two chances to round differently.
//
// `x` must be finite and strictly positive.
bool findScaledDigits(double x, int digits, std::string& outDigits, int& outE) {
    // log10 only has to land near the answer: the loop below corrects it from
    // the DIGIT COUNT of an exactly-computed n, so a floating-point estimate
    // that is off by one costs an iteration and never an answer.
    int e = static_cast<int>(std::floor(std::log10(x)));
    for (int guard = 0; guard < 8; ++guard) {
        std::string n = exactScaledDigits(x, digits - 1 - e);
        const int len = static_cast<int>(n.size());
        if (len == digits) {
            outDigits = std::move(n);
            outE = e;
            return true;
        }
        e += len - digits;
    }
    return false;
}

// 21.1.3.3 Number.prototype.toFixed.
uint64_t numberToFixed(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    double x = 0.0;
    if (!thisNumber(Value(thisBits), "toFixed", x)) return Value::fromUndefined().rawBits();
    const double f = toIntegerOrInfinity(args[0]);
    if (!std::isfinite(f) || f < 0 || f > 100) {
        return rtThrowRangeError("toFixed() digits argument must be between 0 and 100").rawBits();
    }
    if (!std::isfinite(x)) return stringResult(toStringOfNumber(x)).rawBits();

    std::string s;
    // `x < 0` is false for -0, which is 21.1.3.3 step 8 read literally: the
    // sign is taken from ℝ(x), and ℝ(-0) is 0. `(-0).toFixed(2)` is "0.00".
    if (x < 0) {
        s = "-";
        x = -x;
    }
    const int digits = static_cast<int>(f);
    std::string m;
    if (x >= 1e21) {
        // Step 9: the one place the method changes FORMAT rather than
        // precision, because 10^21 is where ToString(Number) leaves positional
        // notation and toFixed has nothing else to fall back on.
        m = toStringOfNumber(x);
    } else {
        m = exactScaledDigits(x, digits);
        if (digits != 0) {
            int k = static_cast<int>(m.size());
            if (k <= digits) {
                m = std::string(static_cast<size_t>(digits + 1 - k), '0') + m;
                k = digits + 1;
            }
            m = m.substr(0, static_cast<size_t>(k - digits)) + "." +
                m.substr(static_cast<size_t>(k - digits));
        }
    }
    return stringResult(s + m).rawBits();
}

// The `e±d` suffix 21.1.3.2 step 12 and 21.1.3.5 step 9.c.iii both build.
std::string exponentSuffix(int e) {
    if (e == 0) return "e+0";
    const std::string d = std::to_string(e < 0 ? -e : e);
    return std::string("e") + (e < 0 ? "-" : "+") + d;
}

// 21.1.3.2 Number.prototype.toExponential.
uint64_t numberToExponential(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    double x = 0.0;
    if (!thisNumber(Value(thisBits), "toExponential", x)) return Value::fromUndefined().rawBits();
    const bool omitted = args[0].isUndefined();
    double f = toIntegerOrInfinity(args[0]);
    // Step 4 precedes step 5, so a non-finite receiver answers before the
    // argument is range-checked: `(NaN).toExponential(500)` is "NaN".
    if (!std::isfinite(x)) return stringResult(toStringOfNumber(x)).rawBits();
    if (!std::isfinite(f) || f < 0 || f > 100) {
        return rtThrowRangeError("toExponential() argument must be between 0 and 100").rawBits();
    }

    std::string s;
    if (x < 0) {
        s = "-";
        x = -x;
    }
    std::string m;
    int e = 0;
    if (x == 0) {
        m = std::string(static_cast<size_t>(f) + 1, '0');
    } else if (omitted) {
        // Step 10.b: the smallest f whose n still round-trips to x, which is
        // the shortest-digits question ToString(Number) already answers.
        char digits[24];
        int count = 0;
        int exp10 = 0;
        jsShortestDigits(x, digits, count, exp10);
        m.assign(digits, static_cast<size_t>(count));
        e = exp10;
        f = count - 1;
    } else if (!findScaledDigits(x, static_cast<int>(f) + 1, m, e)) {
        fatal("internal: toExponential could not place the decimal exponent");
    }
    if (f != 0) m = m.substr(0, 1) + "." + m.substr(1);
    return stringResult(s + m + exponentSuffix(e)).rawBits();
}

// 21.1.3.5 Number.prototype.toPrecision.
uint64_t numberToPrecision(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    double x = 0.0;
    if (!thisNumber(Value(thisBits), "toPrecision", x)) return Value::fromUndefined().rawBits();
    if (args[0].isUndefined()) return stringResult(toStringOfNumber(x)).rawBits();
    const double pRaw = toIntegerOrInfinity(args[0]);
    if (!std::isfinite(x)) return stringResult(toStringOfNumber(x)).rawBits();
    if (!std::isfinite(pRaw) || pRaw < 1 || pRaw > 100) {
        return rtThrowRangeError("toPrecision() argument must be between 1 and 100").rawBits();
    }
    const int p = static_cast<int>(pRaw);

    std::string s;
    if (x < 0) {
        s = "-";
        x = -x;
    }
    std::string m;
    int e = 0;
    if (x == 0) {
        m = std::string(static_cast<size_t>(p), '0');
    } else if (!findScaledDigits(x, p, m, e)) {
        fatal("internal: toPrecision could not place the decimal exponent");
    } else if (e < -6 || e >= p) {
        // Step 9.c: too far from the point in either direction and the result
        // switches to exponential form. `e >= p` is why (123.456).toPrecision(2)
        // is "1.2e+2" and not "120".
        if (p != 1) m = m.substr(0, 1) + "." + m.substr(1);
        return stringResult(s + m + exponentSuffix(e)).rawBits();
    }
    if (e == p - 1) return stringResult(s + m).rawBits();
    if (e >= 0) {
        m = m.substr(0, static_cast<size_t>(e) + 1) + "." + m.substr(static_cast<size_t>(e) + 1);
    } else {
        m = "0." + std::string(static_cast<size_t>(-(e + 1)), '0') + m;
    }
    return stringResult(s + m).rawBits();
}

bool isPowerOfTwo(int r) { return r >= 2 && (r & (r - 1)) == 0; }

// 21.1.3.6 Number.prototype.toString(radix).
//
// Radix 10 is Number::toString and shares its one implementation. Any other
// radix is a different algorithm over the value's own bits, and the spec
// leaves the digit COUNT of a non-terminating fraction implementation-defined
// ("implementation-approximated"). bronze answers only where the answer is
// exact: an integer in any radix, and a fraction in a power-of-two radix,
// where a double's dyadic fraction terminates. A fraction in radix 3 needs a
// shortest-round-trip algorithm in that radix, which bronze has not written,
// so it is a named error rather than a plausible-looking approximation.
uint64_t numberToString(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    double x = 0.0;
    if (!thisNumber(Value(thisBits), "toString", x)) return Value::fromUndefined().rawBits();
    const double radixRaw = args[0].isUndefined() ? 10.0 : toIntegerOrInfinity(args[0]);
    if (!std::isfinite(radixRaw) || radixRaw < 2 || radixRaw > 36) {
        return rtThrowRangeError("toString() radix must be between 2 and 36").rawBits();
    }
    const int radix = static_cast<int>(radixRaw);
    if (radix == 10 || !std::isfinite(x)) return stringResult(toStringOfNumber(x)).rawBits();

    std::string s;
    if (x < 0) {
        s = "-";
        x = -x;
    }
    const std::string whole = exactIntegerDigits(x, radix);
    if (x == std::trunc(x)) return stringResult(s + whole).rawBits();
    if (!isPowerOfTwo(radix)) {
        fatal(("unsupported: Number.prototype.toString(" + std::to_string(radix) +
               ") on a value with a fraction (the digit count of a non-terminating expansion "
               "is not defined by exact arithmetic)")
                  .c_str());
    }
    return stringResult(s + whole + "." + exactDyadicFractionDigits(x, radix)).rawBits();
}

uint64_t numberValueOf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    double x = 0.0;
    if (!thisNumber(Value(thisBits), "valueOf", x)) return Value::fromUndefined().rawBits();
    return Value::fromDouble(x).rawBits();
}

struct NumberMethod {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const NumberMethod kNumberMethods[] = {
    {"toFixed", numberToFixed, 1},
    {"toExponential", numberToExponential, 1},
    {"toPrecision", numberToPrecision, 1},
    {"toString", numberToString, 1},
    {"valueOf", numberValueOf, 0},
};

// Number.prototype members ECMA-262 defines and bronze has not built.
// `toLocaleString` is here for the reason `Math.random` is on Math's list:
// bronze has no locale data and deterministic output is a house rule, so a
// locale-formatted number needs a decision before it can have an
// implementation.
const char* const kNumberProtoMembers[] = {
    "constructor",
    "toLocaleString",
};

}  // namespace

Value rtNumberMethod(const std::string& key) {
    for (const NumberMethod& m : kNumberMethods) {
        if (key == m.name) return rtNativeFunction(m.code, m.arity);
    }
    return Value::fromUndefined();
}

void rtCheckNumberProtoMember(const std::string& key) {
    rtCheckUnimplementedMember("Number.prototype", kNumberProtoMembers,
                               std::size(kNumberProtoMembers), key);
}

}  // namespace bronze::runtime
