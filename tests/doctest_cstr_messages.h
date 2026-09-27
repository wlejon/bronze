// Force-included into every doctest binary when the doctest found is older
// than 2.4.12 (CMakeLists.txt): those versions print a `const char*` message
// or INFO value through their pointer overload, as an address. bronze's tests
// pass their messages as `std::string(...).c_str()`, so on a Linux host whose
// system doctest is 2.4.11 every failure read "0x55..." instead of its text
// (the oracle's "Bronze build failed ...: 0x55..." was the next message, a
// JIT exit-code check, printed that way). A full specialization for C strings
// wins over the library's `filldata<T*>`. It is declared ahead of the library
// header, so only the primary template is named here, exactly as 2.4.11
// declares it.
#pragma once

#include <ostream>

namespace doctest {
namespace detail {

template <typename T>
struct filldata;

template <>
struct filldata<const char*> {
    static void fill(std::ostream* stream, const char* in) { *stream << (in ? in : "(null)"); }
};

template <>
struct filldata<char*> {
    static void fill(std::ostream* stream, const char* in) { *stream << (in ? in : "(null)"); }
};

}  // namespace detail
}  // namespace doctest
