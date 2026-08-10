#include <doctest/doctest.h>

#include <limits>
#include <string>

#include "runtime/number_format.h"

using namespace bronze;

static std::string fmt(double x) {
    char buf[32];
    return std::string(buf, formatJsNumber(x, buf));
}

// Expectations verified against node: each value below prints exactly this
// via String(x) / console.log(x). Pinned per docs/0003 — number formatting
// is the first oracle battleground.
TEST_CASE("formatJsNumber matches JS ToString(Number)") {
    CHECK(fmt(42.0) == "42");
    CHECK(fmt(-42.0) == "-42");
    CHECK(fmt(3000000.0) == "3000000");
    CHECK(fmt(0.0) == "0");
    CHECK(fmt(-0.0) == "0");
    CHECK(fmt(0.5) == "0.5");
    CHECK(fmt(123.456) == "123.456");
    CHECK(fmt(-123.456) == "-123.456");
    CHECK(fmt(0.000001) == "0.000001");
    CHECK(fmt(1e-7) == "1e-7");
    CHECK(fmt(1.5e-8) == "1.5e-8");
    CHECK(fmt(1e20) == "100000000000000000000");
    CHECK(fmt(1e21) == "1e+21");
    CHECK(fmt(1e100) == "1e+100");
    CHECK(fmt(0.1 + 0.2) == "0.30000000000000004");
    CHECK(fmt(1.0 / 3.0) == "0.3333333333333333");
    CHECK(fmt(std::numeric_limits<double>::quiet_NaN()) == "NaN");
    CHECK(fmt(std::numeric_limits<double>::infinity()) == "Infinity");
    CHECK(fmt(-std::numeric_limits<double>::infinity()) == "-Infinity");
}
