#include "runtime/exact_decimal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

#include "runtime/fatal.h"

namespace bronze::runtime {

namespace {

// A non-negative integer of whatever width the question needs, little-endian
// in 32-bit limbs. The widest value any caller here builds is
// m * 10^k * 2^e — about 1400 bits — so the operations are chosen for
// clarity over speed: this runs once per formatted number, never in a loop
// a program can put in a hot path.
class BigUInt {
public:
    BigUInt() = default;
    explicit BigUInt(uint64_t v) {
        limbs_.push_back(static_cast<uint32_t>(v));
        limbs_.push_back(static_cast<uint32_t>(v >> 32));
        trim();
    }

    bool isZero() const noexcept { return limbs_.empty(); }

    void mulSmall(uint32_t factor) {
        if (isZero() || factor == 0) {
            if (factor == 0) limbs_.clear();
            return;
        }
        uint64_t carry = 0;
        for (uint32_t& limb : limbs_) {
            const uint64_t prod = static_cast<uint64_t>(limb) * factor + carry;
            limb = static_cast<uint32_t>(prod);
            carry = prod >> 32;
        }
        while (carry) {
            limbs_.push_back(static_cast<uint32_t>(carry));
            carry >>= 32;
        }
    }

    // Quotient in place, remainder returned. `divisor` must be non-zero.
    uint32_t divSmall(uint32_t divisor) {
        uint64_t rem = 0;
        for (size_t i = limbs_.size(); i-- > 0;) {
            const uint64_t cur = (rem << 32) | limbs_[i];
            limbs_[i] = static_cast<uint32_t>(cur / divisor);
            rem = cur % divisor;
        }
        trim();
        return static_cast<uint32_t>(rem);
    }

    void shiftLeft(uint32_t bits) {
        if (isZero() || bits == 0) return;
        const uint32_t limbShift = bits / 32;
        const uint32_t bitShift = bits % 32;
        std::vector<uint32_t> out(limbs_.size() + limbShift + 1, 0);
        for (size_t i = 0; i < limbs_.size(); ++i) {
            const uint64_t v = static_cast<uint64_t>(limbs_[i]) << bitShift;
            out[i + limbShift] |= static_cast<uint32_t>(v);
            out[i + limbShift + 1] |= static_cast<uint32_t>(v >> 32);
        }
        limbs_ = std::move(out);
        trim();
    }

    void shiftRight(uint32_t bits) {
        if (isZero() || bits == 0) return;
        const uint32_t limbShift = bits / 32;
        const uint32_t bitShift = bits % 32;
        if (limbShift >= limbs_.size()) {
            limbs_.clear();
            return;
        }
        std::vector<uint32_t> out(limbs_.size() - limbShift, 0);
        for (size_t i = 0; i < out.size(); ++i) {
            uint64_t v = limbs_[i + limbShift] >> bitShift;
            if (bitShift != 0 && i + limbShift + 1 < limbs_.size()) {
                v |= static_cast<uint64_t>(limbs_[i + limbShift + 1]) << (32 - bitShift);
            }
            out[i] = static_cast<uint32_t>(v);
        }
        limbs_ = std::move(out);
        trim();
    }

    uint32_t bitLength() const noexcept {
        if (limbs_.empty()) return 0;
        const uint32_t top = limbs_.back();
        return static_cast<uint32_t>((limbs_.size() - 1) * 32 +
                                     (32 - static_cast<uint32_t>(std::countl_zero(top))));
    }

    bool testBit(uint32_t bit) const noexcept {
        const size_t limb = bit / 32;
        if (limb >= limbs_.size()) return false;
        return (limbs_[limb] >> (bit % 32)) & 1u;
    }

    void setBit(uint32_t bit) {
        const size_t limb = bit / 32;
        if (limb >= limbs_.size()) limbs_.resize(limb + 1, 0);
        limbs_[limb] |= 1u << (bit % 32);
    }

    // -1, 0, 1 for less, equal, greater.
    int compare(const BigUInt& other) const noexcept {
        if (limbs_.size() != other.limbs_.size()) {
            return limbs_.size() < other.limbs_.size() ? -1 : 1;
        }
        for (size_t i = limbs_.size(); i-- > 0;) {
            if (limbs_[i] != other.limbs_[i]) return limbs_[i] < other.limbs_[i] ? -1 : 1;
        }
        return 0;
    }

    // `this -= other`, which every caller has already proved non-negative.
    void subtract(const BigUInt& other) {
        int64_t borrow = 0;
        for (size_t i = 0; i < limbs_.size(); ++i) {
            const int64_t rhs = i < other.limbs_.size() ? other.limbs_[i] : 0;
            int64_t diff = static_cast<int64_t>(limbs_[i]) - rhs - borrow;
            if (diff < 0) {
                diff += (int64_t{1} << 32);
                borrow = 1;
            } else {
                borrow = 0;
            }
            limbs_[i] = static_cast<uint32_t>(diff);
        }
        trim();
    }

    void addOne() {
        for (size_t i = 0; i < limbs_.size(); ++i) {
            if (++limbs_[i] != 0) return;
        }
        limbs_.push_back(1);
    }

    void mulPow10(uint32_t exponent) {
        // 10^9 is the largest power of ten a 32-bit limb multiply can take
        // whole, so the exponent is spent nine at a time.
        static const uint32_t kPow10[10] = {1,      10,      100,      1000,      10000,
                                            100000, 1000000, 10000000, 100000000, 1000000000};
        while (exponent >= 9) {
            mulSmall(kPow10[9]);
            exponent -= 9;
        }
        if (exponent > 0) mulSmall(kPow10[exponent]);
    }

    std::string toDecimal() const {
        if (isZero()) return "0";
        BigUInt work = *this;
        std::string out;
        while (!work.isZero()) {
            uint32_t chunk = work.divSmall(1000000000u);
            // Every chunk but the most significant one is nine digits wide,
            // zeros included; the last one drops its leading zeros, which is
            // the whole difference between padding and not.
            const bool mostSignificant = work.isZero();
            for (int i = 0; i < 9 && (!mostSignificant || chunk != 0); ++i) {
                out.push_back(static_cast<char>('0' + chunk % 10));
                chunk /= 10;
            }
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    std::string toRadix(int radix) const {
        if (isZero()) return "0";
        BigUInt work = *this;
        std::string out;
        while (!work.isZero()) {
            const uint32_t digit = work.divSmall(static_cast<uint32_t>(radix));
            out.push_back(digit < 10 ? static_cast<char>('0' + digit)
                                     : static_cast<char>('a' + digit - 10));
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

private:
    void trim() {
        while (!limbs_.empty() && limbs_.back() == 0) limbs_.pop_back();
    }

    std::vector<uint32_t> limbs_;
};

// round(a / b), with a tie going UP — which is "pick the larger n" once the
// sign lives outside (21.1.3.3 step 10, 21.1.3.2 step 10, 21.1.3.5 step 10).
//
// Binary long division rather than Knuth's algorithm D: the quotient is at
// most a few hundred digits, this runs once per formatted value, and the
// schoolbook normalization step is the part of a bignum divide that is
// easiest to get subtly wrong.
BigUInt divideRoundHalfUp(const BigUInt& a, const BigUInt& b) {
    if (b.isZero()) fatal("internal: exact decimal division by zero");
    BigUInt quotient;
    BigUInt rem;
    const uint32_t bits = a.bitLength();
    for (uint32_t i = bits; i-- > 0;) {
        rem.shiftLeft(1);
        if (a.testBit(i)) rem.setBit(0);
        if (rem.compare(b) >= 0) {
            rem.subtract(b);
            quotient.setBit(i);
        }
    }
    // 2 * rem >= b is the tie-or-above test, and doubling the remainder is
    // exact where halving the divisor would not be.
    BigUInt twice = rem;
    twice.shiftLeft(1);
    if (twice.compare(b) >= 0) quotient.addOne();
    return quotient;
}

// Splits a finite double into `mantissa * 2^exponent` with no rounding.
void decompose(double x, uint64_t& mantissa, int& exponent) {
    const uint64_t bits = std::bit_cast<uint64_t>(x);
    const int biased = static_cast<int>((bits >> 52) & 0x7FF);
    const uint64_t frac = bits & ((uint64_t{1} << 52) - 1);
    if (biased == 0) {
        mantissa = frac;
        exponent = -1074;
    } else {
        mantissa = frac | (uint64_t{1} << 52);
        exponent = biased - 1075;
    }
}

}  // namespace

std::string exactScaledDigits(double x, int k) {
    const double mag = std::fabs(x);
    if (mag == 0.0) return "0";
    if (!std::isfinite(mag)) fatal("internal: exact decimal of a non-finite value");

    uint64_t mantissa = 0;
    int exponent = 0;
    decompose(mag, mantissa, exponent);

    // n = round(m * 2^exponent * 10^k) = round(A / B), with every negative
    // power moved into the denominator so both sides stay integers.
    BigUInt a(mantissa);
    BigUInt b(1);
    if (k >= 0) {
        a.mulPow10(static_cast<uint32_t>(k));
    } else {
        b.mulPow10(static_cast<uint32_t>(-k));
    }
    if (exponent >= 0) {
        a.shiftLeft(static_cast<uint32_t>(exponent));
    } else {
        b.shiftLeft(static_cast<uint32_t>(-exponent));
    }
    return divideRoundHalfUp(a, b).toDecimal();
}

std::string exactIntegerDigits(double x, int radix) {
    const double mag = std::fabs(x);
    if (!std::isfinite(mag)) fatal("internal: exact digits of a non-finite value");
    const double whole = std::trunc(mag);
    if (whole == 0.0) return "0";

    uint64_t mantissa = 0;
    int exponent = 0;
    decompose(whole, mantissa, exponent);
    BigUInt value(mantissa);
    if (exponent >= 0) {
        value.shiftLeft(static_cast<uint32_t>(exponent));
    } else {
        value.shiftRight(static_cast<uint32_t>(-exponent));
    }
    return value.toRadix(radix);
}

std::string exactDyadicFractionDigits(double x, int radix) {
    if (radix < 2 || (radix & (radix - 1)) != 0) {
        fatal("internal: a dyadic fraction was asked for in a radix that is not a power of two");
    }
    double fraction = std::fabs(x) - std::trunc(std::fabs(x));
    std::string out;
    // A double's fraction has at most 1074 bits below the point, so one bit
    // per digit is the worst case and the bound is a fact rather than a
    // guess. It is checked because an unbounded loop over floating point is
    // how a formatter hangs.
    for (int guard = 0; fraction != 0.0 && guard < 1100; ++guard) {
        fraction *= radix;  // exact: the radix is a power of two
        const double digit = std::floor(fraction);
        fraction -= digit;
        const int d = static_cast<int>(digit);
        out.push_back(d < 10 ? static_cast<char>('0' + d) : static_cast<char>('a' + d - 10));
    }
    if (fraction != 0.0) fatal("internal: a dyadic fraction failed to terminate");
    return out;
}

}  // namespace bronze::runtime
