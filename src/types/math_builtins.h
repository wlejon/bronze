#pragma once

#include <optional>
#include <string_view>

namespace bronze::types {

// ECMAScript 21.3.2 functions on the pristine Math object that return a Number.
inline bool isMathMethodReturningNumber(std::string_view prop) {
    static constexpr std::string_view kMathFns[] = {
        "abs",     "acos",    "acosh", "asin",  "asinh", "atan",  "atan2", "atanh",
        "cbrt",    "ceil",    "clz32", "cos",   "cosh",  "exp",   "expm1", "floor",
        "fround",  "f16round", "hypot", "imul",  "log",   "log1p", "log10", "log2",
        "max",     "min",     "pow",   "random", "round", "sign",  "sin",   "sinh",
        "sqrt",    "tan",     "tanh",  "trunc"};
    for (const auto& fn : kMathFns) {
        if (prop == fn) return true;
    }
    return false;
}

// ECMAScript 21.3.1 value properties on the pristine Math object: the eight
// Numbers, each non-writable and non-configurable, with the value 21.3.1 gives
// it (the double nearest the real constant, which is what the runtime's Math
// object holds, builtin_math.cpp). The lowering folds a pristine read of one
// to this constant, so both answers come from here and the runtime's table;
// the oracle case math_constants_fold pins them against each other.
inline std::optional<double> mathValueProperty(std::string_view prop) {
    struct Entry { std::string_view name; double value; };
    static constexpr Entry kMathProps[] = {
        {"E", 2.718281828459045},        {"LN10", 2.302585092994046},
        {"LN2", 0.6931471805599453},     {"LOG10E", 0.4342944819032518},
        {"LOG2E", 1.4426950408889634},   {"PI", 3.141592653589793},
        {"SQRT1_2", 0.7071067811865476}, {"SQRT2", 1.4142135623730951}};
    for (const auto& p : kMathProps) {
        if (prop == p.name) return p.value;
    }
    return std::nullopt;
}

inline bool isMathValueProperty(std::string_view prop) {
    return mathValueProperty(prop).has_value();
}

}  // namespace bronze::types
