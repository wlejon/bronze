// The arbitrary-precision core, tested as arithmetic rather than as JS: every
// case here is an identity or an exact value, and nothing in it touches the
// heap or a `Value`. The cases that matter most are the ones a smaller
// implementation gets wrong — the division identity across all four sign
// pairings, the two's-complement bitwise operators at a word boundary, and the
// exact comparison against a double past 2^53.

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "runtime/bignum.h"

using bronze::runtime::BigNum;
using bronze::runtime::BigNumError;

namespace {

BigNum big(const char* decimal) {
    BigNum out;
    REQUIRE(BigNum::parse(decimal, 10, /*allowSign=*/true, out));
    return out;
}

std::string text(const BigNum& v) { return v.toString(10); }

}  // namespace

TEST_CASE("bignum round-trips decimal digits") {
    CHECK(text(BigNum()) == "0");
    CHECK(text(BigNum::fromInt64(0)) == "0");
    CHECK(text(BigNum::fromInt64(-1)) == "-1");
    CHECK(text(BigNum::fromInt64(INT64_MIN)) == "-9223372036854775808");
    CHECK(text(BigNum::fromUint64(UINT64_MAX)) == "18446744073709551615");
    CHECK(text(big("9007199254740993")) == "9007199254740993");
    CHECK(text(big("-340282366920938463463374607431768211456")) ==
          "-340282366920938463463374607431768211456");
}

TEST_CASE("bignum radix round-trips") {
    // Every radix the language admits, both directions, on a value wide enough
    // to need several limbs.
    const BigNum value = big("123456789012345678901234567890123456789");
    for (int radix = 2; radix <= 36; ++radix) {
        const std::string digits = value.toString(radix);
        BigNum parsed;
        REQUIRE(BigNum::parse(digits, radix, /*allowSign=*/true, parsed));
        CHECK(BigNum::compare(parsed, value) == 0);
    }
    CHECK(BigNum::fromInt64(255).toString(16) == "ff");
    CHECK(BigNum::fromInt64(255).toString(2) == "11111111");
    CHECK(BigNum::fromInt64(-255).toString(16) == "-ff");
    CHECK(BigNum::fromInt64(35).toString(36) == "z");
    // An interior zero chunk must survive: 10^18 has nine zeros in the middle
    // of the decimal chunking, and dropping them was the classic bug.
    CHECK(text(big("1000000000000000000")) == "1000000000000000000");
    CHECK(text(big("1000000001000000000")) == "1000000001000000000");
}

TEST_CASE("bignum parse rejects what is not digits") {
    BigNum out;
    CHECK_FALSE(BigNum::parse("", 10, true, out));
    CHECK_FALSE(BigNum::parse("-", 10, true, out));
    CHECK_FALSE(BigNum::parse("12a", 10, true, out));
    CHECK_FALSE(BigNum::parse("19", 8, true, out));
    CHECK_FALSE(BigNum::parse("-5", 10, /*allowSign=*/false, out));
    CHECK(BigNum::parse("ff", 16, true, out));
    CHECK(text(out) == "255");
}

TEST_CASE("bignum addition and multiplication carry across limbs") {
    const BigNum a = BigNum::fromUint64(0xFFFFFFFFULL);
    CHECK(text(BigNum::add(a, BigNum::fromInt64(1))) == "4294967296");
    CHECK(text(BigNum::mul(a, a)) == "18446744065119617025");
    CHECK(text(BigNum::sub(BigNum::fromInt64(1), BigNum::fromInt64(3))) == "-2");
    CHECK(text(BigNum::add(BigNum::fromInt64(-3), BigNum::fromInt64(3))) == "0");
    CHECK(BigNum::add(BigNum::fromInt64(-3), BigNum::fromInt64(3)).isNegative() == false);
    CHECK(text(BigNum::mul(BigNum::fromInt64(-6), BigNum::fromInt64(7))) == "-42");
}

TEST_CASE("bignum division satisfies a == (a/b)*b + a%b") {
    // Every sign pairing and several limb widths, because the identity is what
    // truncation toward zero and a dividend-signed remainder are FOR.
    const char* const operands[] = {
        "0",  "1",   "7",   "-7",  "2",  "-2",  "123456789",  "-123456789",
        "18446744073709551616", "-18446744073709551616",
        "340282366920938463463374607431768211455",
        "-340282366920938463463374607431768211455",
    };
    for (const char* an : operands) {
        for (const char* bn : operands) {
            const BigNum a = big(an);
            const BigNum b = big(bn);
            BigNum q;
            BigNum r;
            if (b.isZero()) {
                CHECK_FALSE(BigNum::divmod(a, b, q, r));
                continue;
            }
            REQUIRE(BigNum::divmod(a, b, q, r));
            const BigNum recomposed = BigNum::add(BigNum::mul(q, b), r);
            CHECK(BigNum::compare(recomposed, a) == 0);
            // |r| < |b|, and r takes the DIVIDEND's sign.
            BigNum absR = r.isNegative() ? BigNum::negate(r) : r;
            BigNum absB = b.isNegative() ? BigNum::negate(b) : b;
            CHECK(BigNum::compare(absR, absB) < 0);
            if (!r.isZero()) CHECK(r.isNegative() == a.isNegative());
        }
    }
}

TEST_CASE("bignum division truncates toward zero") {
    BigNum q;
    BigNum r;
    REQUIRE(BigNum::divmod(big("-7"), big("2"), q, r));
    CHECK(text(q) == "-3");
    CHECK(text(r) == "-1");
    REQUIRE(BigNum::divmod(big("7"), big("-2"), q, r));
    CHECK(text(q) == "-3");
    CHECK(text(r) == "1");
    REQUIRE(BigNum::divmod(big("-7"), big("-2"), q, r));
    CHECK(text(q) == "3");
    CHECK(text(r) == "-1");
}

TEST_CASE("bignum arithmetic agrees with double math on small values") {
    for (int64_t a = -40; a <= 40; ++a) {
        for (int64_t b = -40; b <= 40; ++b) {
            const BigNum x = BigNum::fromInt64(a);
            const BigNum y = BigNum::fromInt64(b);
            CHECK(BigNum::add(x, y).toDouble() == static_cast<double>(a + b));
            CHECK(BigNum::sub(x, y).toDouble() == static_cast<double>(a - b));
            CHECK(BigNum::mul(x, y).toDouble() == static_cast<double>(a * b));
            if (b == 0) continue;
            BigNum q;
            BigNum r;
            REQUIRE(BigNum::divmod(x, y, q, r));
            CHECK(q.toDouble() == static_cast<double>(a / b));
            CHECK(r.toDouble() == static_cast<double>(a % b));
            // C++ and JS both truncate `/` and give `%` the dividend's sign,
            // which is what makes this a cross-check rather than a coincidence.
            CHECK(BigNum::bitAnd(x, y).toDouble() == static_cast<double>(a & b));
            CHECK(BigNum::bitOr(x, y).toDouble() == static_cast<double>(a | b));
            CHECK(BigNum::bitXor(x, y).toDouble() == static_cast<double>(a ^ b));
        }
        CHECK(BigNum::bitNot(BigNum::fromInt64(a)).toDouble() == static_cast<double>(~a));
    }
}

TEST_CASE("bignum bitwise operators use infinite two's complement") {
    CHECK(text(BigNum::bitNot(BigNum::fromInt64(0))) == "-1");
    CHECK(text(BigNum::bitAnd(BigNum::fromInt64(-1), BigNum::fromInt64(255))) == "255");
    CHECK(text(BigNum::bitOr(BigNum::fromInt64(-2), BigNum::fromInt64(1))) == "-1");
    CHECK(text(BigNum::bitXor(BigNum::fromInt64(-1), BigNum::fromInt64(-1))) == "0");
    // At and across a 32-bit word boundary, where a fixed-width implementation
    // stops agreeing with an infinite one.
    CHECK(text(BigNum::bitOr(big("4294967295"), BigNum())) == "4294967295");
    CHECK(text(BigNum::bitAnd(big("-4294967296"), big("4294967295"))) == "0");
    CHECK(text(BigNum::bitAnd(big("-1"), big("4294967296"))) == "4294967296");
    CHECK(text(BigNum::bitXor(big("-4294967296"), big("-1"))) == "4294967295");
    CHECK(text(BigNum::bitOr(big("-4294967297"), big("0"))) == "-4294967297");
    // Two negatives, which is the pairing whose result is negative for AND.
    CHECK(text(BigNum::bitAnd(big("-12345678901234567890"), big("-98765432109876543210"))) ==
          "-110389399237085331194");
}

TEST_CASE("bignum shifts move whole words and parts of words") {
    BigNumError err = BigNumError::None;
    CHECK(text(BigNum::shiftLeft(BigNum::fromInt64(1), BigNum::fromInt64(64), err)) ==
          "18446744073709551616");
    CHECK(err == BigNumError::None);
    // Multiples of the 32-bit word size, where an off-by-one limb shows up as a
    // factor of 2^32.
    for (int words = 0; words <= 5; ++words) {
        const BigNum shifted =
            BigNum::shiftLeft(BigNum::fromInt64(1), BigNum::fromInt64(words * 32), err);
        BigNum expected = BigNum::fromInt64(1);
        for (int i = 0; i < words; ++i) expected = BigNum::mul(expected, BigNum::fromInt64(65536));
        for (int i = 0; i < words; ++i) expected = BigNum::mul(expected, BigNum::fromInt64(65536));
        CHECK(BigNum::compare(shifted, expected) == 0);
        BigNum back = BigNum::shiftRight(shifted, BigNum::fromInt64(words * 32), err);
        CHECK(text(back) == "1");
    }
    // An arithmetic right shift floors, so a negative value never reaches 0n.
    CHECK(text(BigNum::shiftRight(BigNum::fromInt64(-1), BigNum::fromInt64(100), err)) == "-1");
    CHECK(text(BigNum::shiftRight(BigNum::fromInt64(-5), BigNum::fromInt64(1), err)) == "-3");
    CHECK(text(BigNum::shiftRight(BigNum::fromInt64(5), BigNum::fromInt64(1), err)) == "2");
    CHECK(text(BigNum::shiftRight(BigNum::fromInt64(1), BigNum::fromInt64(100), err)) == "0");
    // A negative count is the other shift.
    CHECK(text(BigNum::shiftLeft(BigNum::fromInt64(8), BigNum::fromInt64(-2), err)) == "2");
    CHECK(text(BigNum::shiftRight(BigNum::fromInt64(2), BigNum::fromInt64(-2), err)) == "8");
    // A count no magnitude can reach is a diagnosed size, not an allocation.
    BigNum huge;
    REQUIRE(BigNum::parse("100000000000000000000000", 10, true, huge));
    BigNum::shiftLeft(BigNum::fromInt64(1), huge, err);
    CHECK(err == BigNumError::TooLarge);
    err = BigNumError::None;
    CHECK(text(BigNum::shiftRight(BigNum::fromInt64(-1), huge, err)) == "-1");
}

TEST_CASE("bignum pow") {
    BigNumError err = BigNumError::None;
    CHECK(text(BigNum::pow(BigNum::fromInt64(2), BigNum::fromInt64(128), err)) ==
          "340282366920938463463374607431768211456");
    CHECK(text(BigNum::pow(BigNum::fromInt64(-3), BigNum::fromInt64(3), err)) == "-27");
    CHECK(text(BigNum::pow(BigNum::fromInt64(5), BigNum::fromInt64(0), err)) == "1");
    CHECK(text(BigNum::pow(BigNum::fromInt64(0), BigNum::fromInt64(0), err)) == "1");
    CHECK(err == BigNumError::None);
    BigNum::pow(BigNum::fromInt64(2), BigNum::fromInt64(-1), err);
    CHECK(err == BigNumError::NegativeExponent);
    err = BigNumError::None;
    BigNum::pow(BigNum::fromInt64(2), BigNum::fromInt64(1 << 26), err);
    CHECK(err == BigNumError::TooLarge);
}

TEST_CASE("bignum factorial of 30") {
    BigNum acc = BigNum::fromInt64(1);
    for (int64_t i = 2; i <= 30; ++i) acc = BigNum::mul(acc, BigNum::fromInt64(i));
    CHECK(text(acc) == "265252859812191058636308480000000");
}

TEST_CASE("bignum asIntN and asUintN wrap modulo 2^bits") {
    BigNumError err = BigNumError::None;
    CHECK(text(BigNum::asIntN(8, BigNum::fromInt64(255), err)) == "-1");
    CHECK(text(BigNum::asUintN(8, BigNum::fromInt64(-1), err)) == "255");
    CHECK(text(BigNum::asIntN(8, BigNum::fromInt64(128), err)) == "-128");
    CHECK(text(BigNum::asIntN(8, BigNum::fromInt64(127), err)) == "127");
    CHECK(text(BigNum::asUintN(0, BigNum::fromInt64(-1), err)) == "0");
    CHECK(text(BigNum::asIntN(0, BigNum::fromInt64(-1), err)) == "0");
    CHECK(text(BigNum::asIntN(64, big("18446744073709551615"), err)) == "-1");
    CHECK(text(BigNum::asUintN(64, big("-1"), err)) == "18446744073709551615");
    CHECK(text(BigNum::asIntN(32, big("4294967296"), err)) == "0");
    CHECK(text(BigNum::asIntN(1, BigNum::fromInt64(1), err)) == "-1");
    CHECK(err == BigNumError::None);
}

TEST_CASE("bignum compares exactly against a double") {
    // The whole reason this function exists: at 2^53 the doubles run out of
    // integers and a conversion through one answers the wrong question.
    const BigNum above = big("9007199254740993");
    CHECK(BigNum::compareWithDouble(above, 9007199254740992.0) == 1);
    CHECK(BigNum::compareWithDouble(big("9007199254740992"), 9007199254740992.0) == 0);
    CHECK(BigNum::compareWithDouble(big("9007199254740991"), 9007199254740992.0) == -1);
    CHECK(BigNum::compareWithDouble(BigNum::fromInt64(10), 10.5) == -1);
    CHECK(BigNum::compareWithDouble(BigNum::fromInt64(10), 9.5) == 1);
    CHECK(BigNum::compareWithDouble(BigNum::fromInt64(-10), -10.5) == 1);
    CHECK(BigNum::compareWithDouble(BigNum::fromInt64(-10), -9.5) == -1);
    CHECK(BigNum::compareWithDouble(BigNum(), 0.0) == 0);
    CHECK(BigNum::compareWithDouble(BigNum(), -0.0) == 0);
    CHECK(BigNum::compareWithDouble(BigNum::fromInt64(1),
                                    std::numeric_limits<double>::infinity()) == -1);
    CHECK(BigNum::compareWithDouble(BigNum::fromInt64(1),
                                    -std::numeric_limits<double>::infinity()) == 1);
    CHECK(BigNum::compareWithDouble(BigNum::fromInt64(1),
                                    std::numeric_limits<double>::quiet_NaN()) ==
          BigNum::kUnordered);
    for (int64_t n = -50; n <= 50; ++n) {
        for (double d : {-2.5, -1.0, 0.0, 0.5, 1.0, 40.0, 50.25}) {
            const int expected = static_cast<double>(n) < d ? -1
                                 : (static_cast<double>(n) > d ? 1 : 0);
            CHECK(BigNum::compareWithDouble(BigNum::fromInt64(n), d) == expected);
        }
    }
}

TEST_CASE("bignum converts to a double with round-to-nearest-even") {
    CHECK(BigNum::fromInt64(0).toDouble() == 0.0);
    CHECK(big("9007199254740992").toDouble() == 9007199254740992.0);
    // 2^53 + 1 has no double: the two neighbours are 2^53 and 2^53 + 2, and a
    // tie goes to the even one.
    CHECK(big("9007199254740993").toDouble() == 9007199254740992.0);
    CHECK(big("9007199254740995").toDouble() == 9007199254740996.0);
    CHECK(big("-9007199254740993").toDouble() == -9007199254740992.0);
    BigNumError err = BigNumError::None;
    const BigNum tooBig = BigNum::pow(BigNum::fromInt64(2), BigNum::fromInt64(2000), err);
    CHECK(std::isinf(tooBig.toDouble()));
    CHECK(BigNum::pow(BigNum::fromInt64(2), BigNum::fromInt64(128), err).toDouble() ==
          std::ldexp(1.0, 128));
}

TEST_CASE("bignum reads an integral double exactly") {
    BigNum out;
    CHECK(BigNum::fromDoubleExact(0.0, out));
    CHECK(text(out) == "0");
    CHECK(BigNum::fromDoubleExact(-0.0, out));
    CHECK(text(out) == "0");
    CHECK(BigNum::fromDoubleExact(std::ldexp(1.0, 100), out));
    CHECK(text(out) == "1267650600228229401496703205376");
    CHECK(BigNum::fromDoubleExact(-42.0, out));
    CHECK(text(out) == "-42");
    CHECK_FALSE(BigNum::fromDoubleExact(1.5, out));
    CHECK_FALSE(BigNum::fromDoubleExact(std::numeric_limits<double>::quiet_NaN(), out));
    CHECK_FALSE(BigNum::fromDoubleExact(std::numeric_limits<double>::infinity(), out));
}
