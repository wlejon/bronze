#include <doctest/doctest.h>

#include <string>

#include "runtime/exact_decimal.h"

using namespace bronze::runtime;

// The point of this module is that it does NOT agree with a printf-style
// round: every expectation below is the exact decimal expansion of the double,
// derived from ECMA-262 21.1.3.3's "the integer n for which n / 10^f - x is
// closest to zero, ties to the larger n". Proving that here rather than only
// through the oracle case is what makes a regression name the arithmetic
// instead of a line of console output.
TEST_CASE("exactScaledDigits rounds on the double, not on its shortest form") {
    // The double nearest 1.005 is 1.00499999999999989..., so scaling by 100
    // and rounding gives 100 — printf's "%.2f" answers 101 on some libcs and
    // 100 on others, which is why neither is consulted.
    CHECK(exactScaledDigits(1.005, 2) == "100");
    CHECK(exactScaledDigits(2.675, 2) == "267");
    CHECK(exactScaledDigits(8.575, 2) == "857");
    // A tie that IS exact goes to the larger n, in magnitude terms.
    CHECK(exactScaledDigits(1.5, 0) == "2");
    CHECK(exactScaledDigits(2.5, 0) == "3");
    CHECK(exactScaledDigits(-1.5, 0) == "2");  // the sign lives outside
    CHECK(exactScaledDigits(0.5, 0) == "1");
    CHECK(exactScaledDigits(0.0, 5) == "0");
    CHECK(exactScaledDigits(1.0, 3) == "1000");
    CHECK(exactScaledDigits(1e-7, 2) == "0");
    CHECK(exactScaledDigits(1234.5678, 4) == "12345678");
    CHECK(exactScaledDigits(1234.5678, 0) == "1235");
}

TEST_CASE("exactScaledDigits reaches the full exponent range") {
    // 2^-1074, the smallest subnormal, scaled back up to an integer:
    // 2^-1074 * 10^1080 is 2^6 * 5^1080 exactly, which is 757 digits long and
    // starts 4940656... Anything that went through a double multiply would
    // overflow to infinity long before it got here.
    const std::string tiny = exactScaledDigits(4.9406564584124654e-324, 1080);
    CHECK(tiny.size() == 757);
    CHECK(tiny.substr(0, 7) == "4940656");
    // 2^1023 exactly, which no double multiply survives either.
    const std::string huge = exactScaledDigits(8.98846567431158e307, 0);
    CHECK(huge.size() == 308);
    CHECK(huge.substr(0, 6) == "898846");
    CHECK(huge.back() == '8');
}

TEST_CASE("exactIntegerDigits and exactDyadicFractionDigits") {
    CHECK(exactIntegerDigits(255.0, 16) == "ff");
    CHECK(exactIntegerDigits(255.0, 2) == "11111111");
    CHECK(exactIntegerDigits(-255.0, 16) == "ff");  // magnitude only
    CHECK(exactIntegerDigits(3735928559.0, 16) == "deadbeef");
    CHECK(exactIntegerDigits(0.5, 2) == "0");
    CHECK(exactIntegerDigits(1e21, 10) == "1000000000000000000000");

    CHECK(exactDyadicFractionDigits(0.5, 2) == "1");
    CHECK(exactDyadicFractionDigits(0.25, 2) == "01");
    CHECK(exactDyadicFractionDigits(3.0, 2).empty());
    // 0.1 has no finite binary expansion as a REAL, but the double called 0.1
    // does — 55 significant bits ending at 2^-56 — and that is what a radix-2
    // toString must print.
    const std::string tenth = exactDyadicFractionDigits(0.1, 2);
    CHECK(tenth.substr(0, 8) == "00011001");
    CHECK(tenth.size() == 55);
    CHECK(tenth.back() == '1');
}
