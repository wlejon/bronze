#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bronze::runtime {

// Arbitrary-precision integers, sign and magnitude, with nothing about the JS
// value model in them.
//
// This file is a SEAM and not a runtime component: no heap, no `Value`, no
// exceptions, no allocation the collector can see. That is what lets it be
// tested against arithmetic identities directly (tests/runtime/bignum_test.cpp)
// rather than through a compiled program, and it is why every operation that
// can fail says so in its return rather than raising — the JS error a failure
// becomes (TypeError, RangeError) is 6.1.6.2's business and belongs beside the
// operators, not here.
//
// The magnitude is little-endian 32-bit limbs, normalized so that the top limb
// is never zero and zero is the empty vector. 32 rather than 64 is deliberate:
// every partial product and every division step then fits in a `uint64_t` with
// no compiler-specific 128-bit type, so the same arithmetic runs identically on
// MSVC and on the two Unix toolchains — and identical arithmetic is what the
// deterministic-output rule needs from a numeric core.

enum class BigNumError {
    None,
    // `x / 0n` and `x % 0n`, which 6.1.6.2.5 and 6.1.6.2.6 make a RangeError.
    DivideByZero,
    // `x ** -1n`: 6.1.6.2.3 makes a negative exponent a RangeError, because the
    // mathematical answer is not an integer.
    NegativeExponent,
    // A result whose magnitude passes `kMaxBits`. Not a language limit — the
    // language has none — but a real one, and named rather than hit as a
    // failed allocation halfway through a multiply.
    TooLarge,
    // A digit the radix does not have, or no digits at all.
    BadDigits,
};

class BigNum {
public:
    // The largest magnitude this core will build, in bits. It exists so that
    // `1n << (2n ** 64n)` is a diagnosed RangeError rather than an allocation
    // the process dies inside; a value this size is already 2 MB of limbs.
    static constexpr uint64_t kMaxBits = 1ULL << 24;

    BigNum() = default;

    static BigNum fromInt64(int64_t v);
    static BigNum fromUint64(uint64_t v);
    // The value of `d` exactly, for a `d` that IS a mathematical integer.
    // False for a fraction, a NaN or an infinity — which is the test
    // `BigInt(number)` performs before 7.1.13 will convert one at all.
    static bool fromDoubleExact(double d, BigNum& out);

    bool isZero() const noexcept { return mag_.empty(); }
    bool isNegative() const noexcept { return negative_; }
    int sign() const noexcept { return mag_.empty() ? 0 : (negative_ ? -1 : 1); }
    // Bits in the MAGNITUDE; 0 for zero.
    uint64_t bitLength() const noexcept;

    // -1, 0, 1.
    static int compare(const BigNum& a, const BigNum& b) noexcept;

    // The EXACT comparison against a Number that 7.2.13 and 7.2.14 need, with
    // no conversion through a double in it: 9007199254740993n is greater than
    // the double 9007199254740992, and converting either side to the other's
    // type would answer equal. Returns -1/0/1, or `kUnordered` for a NaN — the
    // third answer IsLessThan has and `==` folds to false.
    static constexpr int kUnordered = 2;
    static int compareWithDouble(const BigNum& a, double d) noexcept;

    // 6.1.6.2's ℝ -> Number: round to nearest, ties to even, and an infinity
    // for a magnitude past the double range. What `Number(bigint)` is.
    double toDouble() const noexcept;

    // The low 64 bits of the magnitude, when the magnitude fits in them. The
    // sign is NOT applied; a caller that wants a signed value asks `isNegative`.
    bool magnitudeToUint64(uint64_t& out) const noexcept;

    static BigNum add(const BigNum& a, const BigNum& b);
    static BigNum sub(const BigNum& a, const BigNum& b);
    static BigNum mul(const BigNum& a, const BigNum& b);
    static BigNum negate(const BigNum& a);

    // Truncating division: the quotient rounds TOWARD ZERO and the remainder
    // takes the DIVIDEND's sign, which is what makes `(-7n) / 2n` be -3n and
    // `(-7n) % 2n` be -1n where a flooring division would answer -4n and 1n.
    // False means `b` is zero and neither output was written.
    static bool divmod(const BigNum& a, const BigNum& b, BigNum& quotient, BigNum& remainder);

    static BigNum pow(const BigNum& base, const BigNum& exponent, BigNumError& err);

    // `<<` and `>>` with a BIGINT count, including a negative one — which each
    // spells the other (6.1.6.2.9, 6.1.6.2.10). `>>` is an ARITHMETIC shift and
    // so is floor division by 2^n: `-1n >> 100n` is -1n, not 0n.
    static BigNum shiftLeft(const BigNum& a, const BigNum& count, BigNumError& err);
    static BigNum shiftRight(const BigNum& a, const BigNum& count, BigNumError& err);

    // The bitwise operators over the INFINITE two's-complement representation
    // 6.1.6.2 defines them on: a negative operand behaves as though it had
    // infinitely many leading 1 bits, so `-1n & 255n` is 255n and `-2n | 1n` is
    // -1n. Never a fixed width, which is what makes these differ from the
    // Number operators of the same spelling.
    static BigNum bitAnd(const BigNum& a, const BigNum& b);
    static BigNum bitOr(const BigNum& a, const BigNum& b);
    static BigNum bitXor(const BigNum& a, const BigNum& b);
    // `~a`, which is exactly `-a - 1`.
    static BigNum bitNot(const BigNum& a);

    // 21.2.2.1 BigInt.asIntN / 21.2.2.2 BigInt.asUintN: the value modulo
    // 2^bits, read as signed or as unsigned. `bits` of 0 is 0n for both.
    static BigNum asIntN(uint64_t bits, const BigNum& a, BigNumError& err);
    static BigNum asUintN(uint64_t bits, const BigNum& a, BigNumError& err);

    // Digits in `radix` (2..36), with an optional leading `+`/`-` only when
    // `allowSign`. StringToBigInt (7.1.14) allows one; a numeric literal's
    // digits never carry one, because `-1n` is a unary minus applied to `1n`.
    static bool parse(std::string_view digits, int radix, bool allowSign, BigNum& out);

    // The digits, with a leading `-` for a negative value. Lower-case above 9,
    // which is what 21.2.3.3 and 6.1.6.2.20 BigInt::toString produce.
    std::string toString(int radix = 10) const;

    const std::vector<uint32_t>& limbs() const noexcept { return mag_; }
    static BigNum fromLimbs(const uint32_t* data, size_t count, bool negative);

private:
    void trim() noexcept;
    // |a| ? |b| as -1/0/1.
    static int compareMagnitude(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) noexcept;
    static std::vector<uint32_t> addMagnitude(const std::vector<uint32_t>& a,
                                              const std::vector<uint32_t>& b);
    // |a| - |b|, which the caller must already know is non-negative.
    static std::vector<uint32_t> subMagnitude(const std::vector<uint32_t>& a,
                                              const std::vector<uint32_t>& b);
    static std::vector<uint32_t> mulMagnitude(const std::vector<uint32_t>& a,
                                              const std::vector<uint32_t>& b);
    // Knuth 4.3.1 algorithm D, and the single-limb short division beside it.
    static void divmodMagnitude(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
                                std::vector<uint32_t>& quotient, std::vector<uint32_t>& remainder);
    static std::vector<uint32_t> shiftLeftBits(const std::vector<uint32_t>& a, uint64_t bits);
    // The magnitude shifted right, with `lostBits` set when anything nonzero
    // fell off the bottom — which is what an arithmetic shift of a NEGATIVE
    // value needs in order to round toward negative infinity.
    static std::vector<uint32_t> shiftRightBits(const std::vector<uint32_t>& a, uint64_t bits,
                                                bool& lostBits);
    // The two's-complement limbs of this value in exactly `count` limbs.
    std::vector<uint32_t> twosComplement(size_t count) const;
    // The inverse, given the sign the caller derived from the operator.
    static BigNum fromTwosComplement(std::vector<uint32_t> limbs, bool negative);

    bool negative_ = false;
    std::vector<uint32_t> mag_;
};

}  // namespace bronze::runtime
