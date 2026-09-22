#pragma once

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

// ECMAScript 21.3.1 value properties on the pristine Math object that are Number.
inline bool isMathValueProperty(std::string_view prop) {
    static constexpr std::string_view kMathProps[] = {
        "PI", "E", "LN2", "LN10", "LOG2E", "LOG10E", "SQRT1_2", "SQRT2"};
    for (const auto& p : kMathProps) {
        if (prop == p) return true;
    }
    return false;
}

}  // namespace bronze::types
