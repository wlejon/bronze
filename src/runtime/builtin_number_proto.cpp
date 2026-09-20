// `Number.prototype`'s members — the four formatting methods and `valueOf`
// (ECMA-262 21.1.3).
//
// Split from builtin_number.cpp along the line ECMA-262 itself draws: that file
// is the `Number` CONSTRUCTOR and the statics of 21.1.2, and this one is
// 21.1.3, the members a NUMBER answers with. The object they are installed on
// is built in builtin_wrappers.cpp beside the other two intrinsic prototypes,
// because 21.1.3 makes it a Number object and that file owns what a wrapper is.
//
// They are installed on that object rather than handed out beside the value,
// which is the difference between a member that can be REACHED and one that can
// only be produced: `Object.getPrototypeOf(1).toFixed` and `(1).toFixed` are one
// function object found by one prototype walk, and a program can hold the
// holder.
//
// All four are defined on the exact real number the double denotes, which is
// why every digit below comes from `exact_decimal.h` and none from printf or a
// to_chars round-trip: `(1.005).toFixed(2)` is "1.00" and any implementation
// that answers "1.01" is wrong in exactly the code that calls toFixed.

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exact_decimal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/number_format.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// 21.1.3's thisNumberValue: the primitive itself, or a Number object's
// [[NumberData]]. Anything else is the TypeError the clause names, and it is
// reachable — `Number.prototype.toFixed.call("x")` is a program detaching the
// method from its receiver.
bool thisNumber(Value self, const char* method, double& out) {
    Value number;
    if (!rtThisNumberValue(self, number)) {
        rtThrowTypeError(std::string("Number.prototype.") + method +
                         " called on a value that is not a number");
        return false;
    }
    out = number.asNumber();
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

// The fraction of |x| in a radix where its expansion does not terminate,
// with the digit count V8 uses (DoubleToRadixCString): emit digits while the
// remaining fraction is still above half the gap between `x` and the next
// double, rounding the last digit half-to-even and carrying into the digits
// already written. That is the shortest string that pins the double at its
// own precision, and it is byte-for-byte what Chromium prints, which is the
// answer `Math.random().toString(36).slice(2)` and every id generator built
// on it expect. Each step is one IEEE multiply by a small integer, so the
// digits are deterministic on every platform. `carry` reports the one case
// where rounding overflowed the fraction entirely and the integer part is one
// more than trunc(x).
std::string radixFractionDigits(double x, int radix, bool& carry) {
    static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    const double integer = std::floor(x);
    double fraction = x - integer;
    double delta = 0.5 * (std::nextafter(x, INFINITY) - x);
    delta = std::max(std::nextafter(0.0, 1.0), delta);
    std::string out;
    carry = false;
    if (fraction < delta) return out;
    do {
        fraction *= radix;
        delta *= radix;
        int digit = static_cast<int>(fraction);
        out.push_back(kDigits[digit]);
        fraction -= digit;
        if (fraction > 0.5 || (fraction == 0.5 && (digit & 1))) {
            if (fraction + delta > 1) {
                // Round up, propagating the carry through the digits written.
                while (true) {
                    if (out.empty()) {
                        carry = true;
                        break;
                    }
                    const char c = out.back();
                    out.pop_back();
                    const int d = c > '9' ? (c - 'a' + 10) : (c - '0');
                    if (d + 1 < radix) {
                        out.push_back(kDigits[d + 1]);
                        break;
                    }
                }
                break;
            }
        }
    } while (fraction >= delta);
    return out;
}

// 21.1.3.6 Number.prototype.toString(radix).
//
// Radix 10 is Number::toString and shares its one implementation. Any other
// radix is a different algorithm over the value's own bits, and the spec
// leaves the digit COUNT of a non-terminating fraction implementation-defined
// ("implementation-approximated"). The integer part is exact in any radix,
// and so is the fraction in a power-of-two radix, where a double's dyadic
// fraction terminates. Any other radix takes V8's digit count above, so the
// spelling agrees with Chromium rather than being merely plausible.
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
    if (x == std::trunc(x)) return stringResult(s + exactIntegerDigits(x, radix)).rawBits();
    if (isPowerOfTwo(radix)) {
        return stringResult(s + exactIntegerDigits(x, radix) + "." + exactDyadicFractionDigits(x, radix))
            .rawBits();
    }
    bool carry = false;
    const std::string fraction = radixFractionDigits(x, radix, carry);
    const std::string whole = exactIntegerDigits(carry ? std::trunc(x) + 1.0 : x, radix);
    if (fraction.empty()) return stringResult(s + whole).rawBits();
    return stringResult(s + whole + "." + fraction).rawBits();
}

uint64_t numberValueOf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    double x = 0.0;
    if (!thisNumber(Value(thisBits), "valueOf", x)) return Value::fromUndefined().rawBits();
    return Value::fromDouble(x).rawBits();
}

uint64_t numberToLocaleString(uint64_t code, uint64_t thisBits, uint32_t, const uint64_t*) {
    return numberToString(code, thisBits, 0, nullptr);
}

const NativeMethod kNumberProtoMethods[] = {
    {"toFixed", numberToFixed, 1, 1},
    {"toExponential", numberToExponential, 1, 1},
    {"toPrecision", numberToPrecision, 1, 1},
    {"toString", numberToString, 1, 1},
    {"toLocaleString", numberToLocaleString, 0, 0},
    {"valueOf", numberValueOf, 0, 0},
};

const char* const kNumberProtoMembers[] = {nullptr};

}  // namespace

void rtInstallNumberMethods(Rooted<Value>& proto) {
    rtDefineMethods(proto, kNumberProtoMethods, std::size(kNumberProtoMethods));
}

void rtCheckNumberProtoMember(const std::string& key) {
    rtCheckUnimplementedMember("Number.prototype", kNumberProtoMembers,
                               sizeof(kNumberProtoMembers) / sizeof(const char*), key);
}

}  // namespace bronze::runtime
