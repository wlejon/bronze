#pragma once

// What String.prototype's two plain-member files share: builtin_string.cpp
// (the table, and the members that copy a string to units) and
// builtin_string_search.cpp (the members that read a string in place --
// the readers, the searches, slice and substring -- and their fast entries).
// The members a compiled site enters directly are extern "C" in
// native_fast.h; the rest are here.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace bronze::runtime::string_detail {

// 7.1.5 ToIntegerOrInfinity of a Number already converted.
inline double toInteger(double d) {
    if (std::isnan(d)) return 0.0;
    if (std::isinf(d)) return d;
    double t = std::trunc(d);
    return t == 0.0 ? 0.0 : t;
}

// A relative index (slice's): a negative one counts from the end.
inline uint32_t relativeIndex(double rel, size_t len) {
    if (rel < 0) {
        double from = static_cast<double>(len) + rel;
        return from < 0 ? 0u : static_cast<uint32_t>(from);
    }
    double capped = std::min(rel, static_cast<double>(len));
    return static_cast<uint32_t>(capped);
}

// A clamp with no negative meaning, which is what substring uses (and the
// reason substring and slice disagree on negative arguments).
inline uint32_t clampIndex(double v, size_t len) {
    if (std::isnan(v) || v < 0) return 0;
    if (v > static_cast<double>(len)) return static_cast<uint32_t>(len);
    return static_cast<uint32_t>(v);
}

uint64_t stringAt(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t stringCodePointAt(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t stringLastIndexOf(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t stringIncludes(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t stringEndsWith(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t stringSubstr(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

}  // namespace bronze::runtime::string_detail
