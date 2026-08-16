#include "runtime/bignum.h"

#include <algorithm>
#include <cmath>

namespace bronze::runtime {

namespace {

constexpr uint64_t kBase = 1ULL << 32;

// The bit index of the highest set bit of a nonzero limb, plus one.
uint32_t limbBits(uint32_t v) noexcept {
    uint32_t n = 0;
    while (v) {
        ++n;
        v >>= 1;
    }
    return n;
}

int digitValue(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

// magnitude = magnitude * multiplier + addend, in place. The one primitive the
// radix parser needs, and the same one `fromUint64` of a large value would use.
void mulAddSmall(std::vector<uint32_t>& mag, uint32_t multiplier, uint32_t addend) {
    uint64_t carry = addend;
    for (uint32_t& limb : mag) {
        const uint64_t product = static_cast<uint64_t>(limb) * multiplier + carry;
        limb = static_cast<uint32_t>(product);
        carry = product >> 32;
    }
    while (carry) {
        mag.push_back(static_cast<uint32_t>(carry));
        carry >>= 32;
    }
}

// magnitude /= divisor, returning the remainder. Short division, most
// significant limb first.
uint32_t divModSmall(std::vector<uint32_t>& mag, uint32_t divisor) {
    uint64_t rem = 0;
    for (size_t i = mag.size(); i-- > 0;) {
        const uint64_t cur = (rem << 32) | mag[i];
        mag[i] = static_cast<uint32_t>(cur / divisor);
        rem = cur % divisor;
    }
    while (!mag.empty() && mag.back() == 0) mag.pop_back();
    return static_cast<uint32_t>(rem);
}

// The largest power of `radix` that fits in a uint32_t, and its exponent. Used
// so that `toString` divides once per NINE decimal digits rather than once per
// digit — the difference between O(n^2) with a small constant and one with a
// large one, on a value the size of `factorial(30n)` or `2n ** 128n`.
struct RadixChunk {
    uint32_t power;
    uint32_t digits;
};

RadixChunk radixChunk(int radix) noexcept {
    RadixChunk chunk{static_cast<uint32_t>(radix), 1};
    while (true) {
        const uint64_t next = static_cast<uint64_t>(chunk.power) * static_cast<uint32_t>(radix);
        if (next > 0xFFFFFFFFULL) break;
        chunk.power = static_cast<uint32_t>(next);
        ++chunk.digits;
    }
    return chunk;
}

}  // namespace

void BigNum::trim() noexcept {
    while (!mag_.empty() && mag_.back() == 0) mag_.pop_back();
    // There is one zero, and it is not negative: `0n === -0n` is true, so a
    // negative zero here would make two values that must be identical compare
    // unequal on the sign word alone.
    if (mag_.empty()) negative_ = false;
}

BigNum BigNum::fromUint64(uint64_t v) {
    BigNum out;
    if (v != 0) {
        out.mag_.push_back(static_cast<uint32_t>(v));
        if (v >> 32) out.mag_.push_back(static_cast<uint32_t>(v >> 32));
    }
    return out;
}

BigNum BigNum::fromInt64(int64_t v) {
    // Through the unsigned magnitude, because negating INT64_MIN is undefined
    // behaviour and it is exactly the value a 64-bit round-trip must carry.
    const uint64_t magnitude =
        v < 0 ? (~static_cast<uint64_t>(v) + 1ULL) : static_cast<uint64_t>(v);
    BigNum out = fromUint64(magnitude);
    out.negative_ = v < 0 && !out.mag_.empty();
    return out;
}

BigNum BigNum::fromLimbs(const uint32_t* data, size_t count, bool negative) {
    BigNum out;
    out.mag_.assign(data, data + count);
    out.negative_ = negative;
    out.trim();
    return out;
}

bool BigNum::fromDoubleExact(double d, BigNum& out) {
    if (!std::isfinite(d)) return false;
    if (d != std::trunc(d)) return false;
    const bool negative = std::signbit(d) && d != 0.0;
    double magnitude = std::fabs(d);
    out = BigNum();
    if (magnitude == 0.0) return true;
    // d = mantissa * 2^exponent with the mantissa an exact 53-bit integer, so
    // the value is a shift of a uint64 and no digit is ever approximated.
    int exponent = 0;
    const double fraction = std::frexp(magnitude, &exponent);
    const uint64_t mantissa = static_cast<uint64_t>(std::ldexp(fraction, 53));
    const int shift = exponent - 53;
    out = fromUint64(mantissa);
    if (shift > 0) {
        out.mag_ = shiftLeftBits(out.mag_, static_cast<uint64_t>(shift));
    } else if (shift < 0) {
        bool lost = false;
        out.mag_ = shiftRightBits(out.mag_, static_cast<uint64_t>(-shift), lost);
        // An integral double whose mantissa has trailing zeros below the binary
        // point: the shift discards them and nothing is lost. `d ==
        // std::trunc(d)` above is what guarantees that.
        (void)lost;
    }
    out.negative_ = negative;
    out.trim();
    return true;
}

uint64_t BigNum::bitLength() const noexcept {
    if (mag_.empty()) return 0;
    return static_cast<uint64_t>(mag_.size() - 1) * 32 + limbBits(mag_.back());
}

int BigNum::compareMagnitude(const std::vector<uint32_t>& a,
                             const std::vector<uint32_t>& b) noexcept {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

int BigNum::compare(const BigNum& a, const BigNum& b) noexcept {
    if (a.negative_ != b.negative_) return a.negative_ ? -1 : 1;
    const int magnitudeOrder = compareMagnitude(a.mag_, b.mag_);
    return a.negative_ ? -magnitudeOrder : magnitudeOrder;
}

std::vector<uint32_t> BigNum::addMagnitude(const std::vector<uint32_t>& a,
                                           const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out;
    const size_t n = std::max(a.size(), b.size());
    out.reserve(n + 1);
    uint64_t carry = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t sum = (i < a.size() ? a[i] : 0ULL) + (i < b.size() ? b[i] : 0ULL) + carry;
        out.push_back(static_cast<uint32_t>(sum));
        carry = sum >> 32;
    }
    if (carry) out.push_back(static_cast<uint32_t>(carry));
    return out;
}

std::vector<uint32_t> BigNum::subMagnitude(const std::vector<uint32_t>& a,
                                           const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out;
    out.reserve(a.size());
    int64_t borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        int64_t diff = static_cast<int64_t>(a[i]) - (i < b.size() ? b[i] : 0U) - borrow;
        if (diff < 0) {
            diff += static_cast<int64_t>(kBase);
            borrow = 1;
        } else {
            borrow = 0;
        }
        out.push_back(static_cast<uint32_t>(diff));
    }
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

std::vector<uint32_t> BigNum::mulMagnitude(const std::vector<uint32_t>& a,
                                           const std::vector<uint32_t>& b) {
    if (a.empty() || b.empty()) return {};
    std::vector<uint32_t> out(a.size() + b.size(), 0);
    for (size_t i = 0; i < a.size(); ++i) {
        uint64_t carry = 0;
        const uint64_t ai = a[i];
        if (ai == 0) continue;
        for (size_t j = 0; j < b.size(); ++j) {
            const uint64_t cur = out[i + j] + ai * b[j] + carry;
            out[i + j] = static_cast<uint32_t>(cur);
            carry = cur >> 32;
        }
        size_t k = i + b.size();
        while (carry) {
            const uint64_t cur = out[k] + carry;
            out[k] = static_cast<uint32_t>(cur);
            carry = cur >> 32;
            ++k;
        }
    }
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

BigNum BigNum::add(const BigNum& a, const BigNum& b) {
    BigNum out;
    if (a.negative_ == b.negative_) {
        out.mag_ = addMagnitude(a.mag_, b.mag_);
        out.negative_ = a.negative_;
    } else {
        const int order = compareMagnitude(a.mag_, b.mag_);
        if (order == 0) return BigNum();
        if (order > 0) {
            out.mag_ = subMagnitude(a.mag_, b.mag_);
            out.negative_ = a.negative_;
        } else {
            out.mag_ = subMagnitude(b.mag_, a.mag_);
            out.negative_ = b.negative_;
        }
    }
    out.trim();
    return out;
}

BigNum BigNum::negate(const BigNum& a) {
    BigNum out = a;
    out.negative_ = !out.negative_;
    out.trim();
    return out;
}

BigNum BigNum::sub(const BigNum& a, const BigNum& b) { return add(a, negate(b)); }

BigNum BigNum::mul(const BigNum& a, const BigNum& b) {
    BigNum out;
    out.mag_ = mulMagnitude(a.mag_, b.mag_);
    out.negative_ = a.negative_ != b.negative_;
    out.trim();
    return out;
}

// Knuth 4.3.1 algorithm D. The normalization shift is what makes the trial
// quotient below correct: with the divisor's top limb at or above half the
// base, `qhat` is at most two too large, and the two correction steps that
// follow are then exact.
void BigNum::divmodMagnitude(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
                             std::vector<uint32_t>& quotient, std::vector<uint32_t>& remainder) {
    quotient.clear();
    remainder.clear();
    if (compareMagnitude(a, b) < 0) {
        remainder = a;
        return;
    }
    if (b.size() == 1) {
        std::vector<uint32_t> work = a;
        const uint32_t rem = divModSmall(work, b[0]);
        quotient = std::move(work);
        if (rem != 0) remainder.push_back(rem);
        return;
    }

    const uint32_t shift = 32 - limbBits(b.back());
    std::vector<uint32_t> u = shiftLeftBits(a, shift);
    const std::vector<uint32_t> v = shiftLeftBits(b, shift);
    const size_t n = v.size();
    const size_t m = u.size() >= n ? u.size() - n : 0;
    // One extra limb above the dividend, which algorithm D indexes as u[j+n].
    u.push_back(0);

    quotient.assign(m + 1, 0);
    const uint64_t vHigh = v[n - 1];
    const uint64_t vNext = v[n - 2];

    for (size_t j = m + 1; j-- > 0;) {
        const uint64_t numerator = (static_cast<uint64_t>(u[j + n]) << 32) | u[j + n - 1];
        uint64_t qhat = numerator / vHigh;
        uint64_t rhat = numerator % vHigh;
        if (qhat > 0xFFFFFFFFULL) {
            qhat = 0xFFFFFFFFULL;
            rhat = numerator - qhat * vHigh;
        }
        while (rhat <= 0xFFFFFFFFULL && qhat * vNext > ((rhat << 32) | u[j + n - 2])) {
            --qhat;
            rhat += vHigh;
        }

        // u[j..j+n] -= qhat * v, as one pass with a running borrow.
        int64_t borrow = 0;
        uint64_t carry = 0;
        for (size_t i = 0; i < n; ++i) {
            const uint64_t product = qhat * v[i] + carry;
            carry = product >> 32;
            int64_t diff = static_cast<int64_t>(u[i + j]) - static_cast<int64_t>(product & 0xFFFFFFFFULL) - borrow;
            if (diff < 0) {
                diff += static_cast<int64_t>(kBase);
                borrow = 1;
            } else {
                borrow = 0;
            }
            u[i + j] = static_cast<uint32_t>(diff);
        }
        int64_t topDiff = static_cast<int64_t>(u[j + n]) - static_cast<int64_t>(carry) - borrow;
        if (topDiff < 0) {
            // qhat was one too large: add the divisor back and step down. The
            // trial-quotient bound makes this rare, and it is the only place a
            // wrong guess is repaired.
            topDiff += static_cast<int64_t>(kBase);
            --qhat;
            uint64_t addCarry = 0;
            for (size_t i = 0; i < n; ++i) {
                const uint64_t sum = static_cast<uint64_t>(u[i + j]) + v[i] + addCarry;
                u[i + j] = static_cast<uint32_t>(sum);
                addCarry = sum >> 32;
            }
            topDiff += static_cast<int64_t>(addCarry);
        }
        u[j + n] = static_cast<uint32_t>(topDiff);
        quotient[j] = static_cast<uint32_t>(qhat);
    }

    while (!quotient.empty() && quotient.back() == 0) quotient.pop_back();
    u.resize(n);
    bool lost = false;
    remainder = shiftRightBits(u, shift, lost);
}

bool BigNum::divmod(const BigNum& a, const BigNum& b, BigNum& quotient, BigNum& remainder) {
    if (b.isZero()) return false;
    std::vector<uint32_t> q;
    std::vector<uint32_t> r;
    divmodMagnitude(a.mag_, b.mag_, q, r);
    quotient = BigNum();
    quotient.mag_ = std::move(q);
    // Truncation toward zero: the quotient's sign is the operands' XOR and the
    // remainder keeps the DIVIDEND's, which together satisfy
    // a = (a / b) * b + a % b for every pair of signs.
    quotient.negative_ = a.negative_ != b.negative_;
    quotient.trim();
    remainder = BigNum();
    remainder.mag_ = std::move(r);
    remainder.negative_ = a.negative_;
    remainder.trim();
    return true;
}

BigNum BigNum::pow(const BigNum& base, const BigNum& exponent, BigNumError& err) {
    err = BigNumError::None;
    if (exponent.isNegative()) {
        err = BigNumError::NegativeExponent;
        return BigNum();
    }
    uint64_t e = 0;
    if (!exponent.magnitudeToUint64(e)) {
        err = BigNumError::TooLarge;
        return BigNum();
    }
    if (e == 0) return fromUint64(1);
    if (base.isZero()) return BigNum();
    // The result's size is known before a single limb is multiplied, so an
    // exponent that would overflow the cap is named here rather than found
    // partway through the squaring.
    const uint64_t bits = base.bitLength();
    if (bits > 1 && bits * e > kMaxBits) {
        err = BigNumError::TooLarge;
        return BigNum();
    }
    if (bits == 1 && e > kMaxBits) {
        // |base| is 1: the magnitude never grows, only the sign alternates.
        BigNum out = base;
        out.negative_ = base.negative_ && (e % 2 == 1);
        return out;
    }

    BigNum result = fromUint64(1);
    BigNum square = base;
    while (e > 0) {
        if (e & 1ULL) result = mul(result, square);
        e >>= 1;
        if (e > 0) square = mul(square, square);
    }
    return result;
}

std::vector<uint32_t> BigNum::shiftLeftBits(const std::vector<uint32_t>& a, uint64_t bits) {
    if (a.empty()) return {};
    const size_t limbShift = static_cast<size_t>(bits / 32);
    const uint32_t bitShift = static_cast<uint32_t>(bits % 32);
    std::vector<uint32_t> out(a.size() + limbShift + 1, 0);
    for (size_t i = 0; i < a.size(); ++i) {
        const uint64_t shifted = static_cast<uint64_t>(a[i]) << bitShift;
        out[i + limbShift] |= static_cast<uint32_t>(shifted);
        out[i + limbShift + 1] |= static_cast<uint32_t>(shifted >> 32);
    }
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

std::vector<uint32_t> BigNum::shiftRightBits(const std::vector<uint32_t>& a, uint64_t bits,
                                             bool& lostBits) {
    lostBits = false;
    const uint64_t limbShift64 = bits / 32;
    if (limbShift64 >= a.size()) {
        for (uint32_t limb : a) {
            if (limb != 0) lostBits = true;
        }
        return {};
    }
    const size_t limbShift = static_cast<size_t>(limbShift64);
    const uint32_t bitShift = static_cast<uint32_t>(bits % 32);
    for (size_t i = 0; i < limbShift; ++i) {
        if (a[i] != 0) lostBits = true;
    }
    if (bitShift != 0 && (a[limbShift] << (32 - bitShift)) != 0) lostBits = true;

    std::vector<uint32_t> out(a.size() - limbShift, 0);
    for (size_t i = 0; i < out.size(); ++i) {
        uint64_t value = a[i + limbShift] >> bitShift;
        if (bitShift != 0 && i + limbShift + 1 < a.size()) {
            value |= static_cast<uint64_t>(a[i + limbShift + 1]) << (32 - bitShift);
        }
        out[i] = static_cast<uint32_t>(value);
    }
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

BigNum BigNum::shiftLeft(const BigNum& a, const BigNum& count, BigNumError& err) {
    err = BigNumError::None;
    if (count.isNegative()) return shiftRight(a, negate(count), err);
    if (a.isZero()) return BigNum();
    uint64_t bits = 0;
    if (!count.magnitudeToUint64(bits) || a.bitLength() + bits > kMaxBits) {
        err = BigNumError::TooLarge;
        return BigNum();
    }
    BigNum out;
    out.mag_ = shiftLeftBits(a.mag_, bits);
    out.negative_ = a.negative_;
    out.trim();
    return out;
}

BigNum BigNum::shiftRight(const BigNum& a, const BigNum& count, BigNumError& err) {
    err = BigNumError::None;
    if (count.isNegative()) return shiftLeft(a, negate(count), err);
    if (a.isZero()) return BigNum();
    uint64_t bits = 0;
    if (!count.magnitudeToUint64(bits)) {
        // A shift count past any magnitude: everything is shifted out, and an
        // ARITHMETIC shift of a negative value floors to -1n rather than to 0n.
        return a.negative_ ? fromInt64(-1) : BigNum();
    }
    bool lost = false;
    BigNum out;
    out.mag_ = shiftRightBits(a.mag_, bits, lost);
    out.negative_ = a.negative_;
    out.trim();
    if (a.negative_ && lost) {
        // floor(-m / 2^n) is -(ceil(m / 2^n)), which is one MORE in magnitude
        // than the truncated shift whenever a set bit fell off the bottom.
        out = sub(out, fromUint64(1));
    }
    return out;
}

std::vector<uint32_t> BigNum::twosComplement(size_t count) const {
    std::vector<uint32_t> out(count, 0);
    for (size_t i = 0; i < count && i < mag_.size(); ++i) out[i] = mag_[i];
    if (!negative_) return out;
    uint64_t carry = 1;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t inverted = static_cast<uint64_t>(static_cast<uint32_t>(~out[i])) + carry;
        out[i] = static_cast<uint32_t>(inverted);
        carry = inverted >> 32;
    }
    return out;
}

BigNum BigNum::fromTwosComplement(std::vector<uint32_t> limbs, bool negative) {
    BigNum out;
    if (negative) {
        uint64_t carry = 1;
        for (uint32_t& limb : limbs) {
            const uint64_t inverted = static_cast<uint64_t>(static_cast<uint32_t>(~limb)) + carry;
            limb = static_cast<uint32_t>(inverted);
            carry = inverted >> 32;
        }
    }
    out.mag_ = std::move(limbs);
    out.negative_ = negative;
    out.trim();
    return out;
}

// The three binary bitwise operators differ in one line each. What they share
// is the part that is easy to get wrong: how wide the two's-complement window
// has to be. One limb ABOVE the wider magnitude, because a positive value of L
// limbs needs L+1 for its sign limb to read as zero — without it,
// `0xFFFFFFFFn | 0n` would come back negative. The result's sign is derived
// from the operator rather than read off the top limb, which is the same fact
// stated where it is decidable.
BigNum BigNum::bitAnd(const BigNum& a, const BigNum& b) {
    const size_t count = std::max(a.mag_.size(), b.mag_.size()) + 1;
    std::vector<uint32_t> lhs = a.twosComplement(count);
    const std::vector<uint32_t> rhs = b.twosComplement(count);
    for (size_t i = 0; i < count; ++i) lhs[i] &= rhs[i];
    return fromTwosComplement(std::move(lhs), a.negative_ && b.negative_);
}

BigNum BigNum::bitOr(const BigNum& a, const BigNum& b) {
    const size_t count = std::max(a.mag_.size(), b.mag_.size()) + 1;
    std::vector<uint32_t> lhs = a.twosComplement(count);
    const std::vector<uint32_t> rhs = b.twosComplement(count);
    for (size_t i = 0; i < count; ++i) lhs[i] |= rhs[i];
    return fromTwosComplement(std::move(lhs), a.negative_ || b.negative_);
}

BigNum BigNum::bitXor(const BigNum& a, const BigNum& b) {
    const size_t count = std::max(a.mag_.size(), b.mag_.size()) + 1;
    std::vector<uint32_t> lhs = a.twosComplement(count);
    const std::vector<uint32_t> rhs = b.twosComplement(count);
    for (size_t i = 0; i < count; ++i) lhs[i] ^= rhs[i];
    return fromTwosComplement(std::move(lhs), a.negative_ != b.negative_);
}

BigNum BigNum::bitNot(const BigNum& a) { return sub(fromInt64(-1), a); }

BigNum BigNum::asUintN(uint64_t bits, const BigNum& a, BigNumError& err) {
    err = BigNumError::None;
    if (bits == 0) return BigNum();
    if (bits > kMaxBits) {
        err = BigNumError::TooLarge;
        return BigNum();
    }
    // The value modulo 2^bits, which for a NEGATIVE operand is the two's
    // -complement window read as unsigned — so `BigInt.asUintN(8, -1n)` is 255n.
    const size_t count = static_cast<size_t>((bits + 31) / 32);
    std::vector<uint32_t> limbs = a.twosComplement(std::max(count, a.mag_.size() + 1));
    limbs.resize(count);
    const uint32_t topBits = static_cast<uint32_t>(bits % 32);
    if (topBits != 0) limbs.back() &= (1U << topBits) - 1U;
    BigNum out;
    out.mag_ = std::move(limbs);
    out.negative_ = false;
    out.trim();
    return out;
}

BigNum BigNum::asIntN(uint64_t bits, const BigNum& a, BigNumError& err) {
    BigNum wrapped = asUintN(bits, a, err);
    if (err != BigNumError::None || bits == 0) return wrapped;
    // 21.2.2.1 step 4: at or above 2^(bits-1) the window denotes a negative
    // value, and the signed answer is that window minus 2^bits.
    BigNumError shiftErr = BigNumError::None;
    const BigNum half = shiftLeft(fromUint64(1), fromUint64(bits - 1), shiftErr);
    if (shiftErr != BigNumError::None) {
        err = shiftErr;
        return BigNum();
    }
    if (compare(wrapped, half) < 0) return wrapped;
    const BigNum whole = shiftLeft(fromUint64(1), fromUint64(bits), shiftErr);
    if (shiftErr != BigNumError::None) {
        err = shiftErr;
        return BigNum();
    }
    return sub(wrapped, whole);
}

bool BigNum::magnitudeToUint64(uint64_t& out) const noexcept {
    if (mag_.size() > 2) return false;
    out = 0;
    if (mag_.size() >= 1) out |= mag_[0];
    if (mag_.size() == 2) out |= static_cast<uint64_t>(mag_[1]) << 32;
    return true;
}

int BigNum::compareWithDouble(const BigNum& a, double d) noexcept {
    if (std::isnan(d)) return kUnordered;
    if (std::isinf(d)) return d > 0 ? -1 : 1;
    // The sign decides whenever the two differ, and it decides BEFORE any
    // magnitude is built — which is also what makes `0n == -0` true, since
    // both sides then have sign 0.
    const int aSign = a.sign();
    const int dSign = d > 0 ? 1 : (d < 0 ? -1 : 0);
    if (aSign != dSign) return aSign < dSign ? -1 : 1;
    if (aSign == 0) return 0;

    // Same sign, so the answer is the magnitude comparison with the sign
    // applied. The double's integer part is exact and its fraction is the tie
    // break: equal integer parts with a nonzero fraction means the double is
    // the larger magnitude.
    const double magnitude = std::fabs(d);
    double integral = 0.0;
    const double fraction = std::modf(magnitude, &integral);
    BigNum truncated;
    if (!fromDoubleExact(integral, truncated)) return kUnordered;
    int order = compareMagnitude(a.mag_, truncated.mag_);
    if (order == 0 && fraction != 0.0) order = -1;
    return aSign < 0 ? -order : order;
}

double BigNum::toDouble() const noexcept {
    if (mag_.empty()) return 0.0;
    const uint64_t bits = bitLength();
    double magnitude = 0.0;
    if (bits <= 53) {
        uint64_t value = 0;
        magnitudeToUint64(value);
        magnitude = static_cast<double>(value);
    } else {
        // Round to nearest, ties to even, over the exact bits: take the top 54,
        // let the 54th be the round bit, and let any bit below it be sticky.
        const uint64_t drop = bits - 54;
        bool sticky = false;
        const std::vector<uint32_t> top = shiftRightBits(mag_, drop, sticky);
        uint64_t value = 0;
        // `top` is exactly 54 bits, so it always fits.
        if (top.size() >= 1) value |= top[0];
        if (top.size() >= 2) value |= static_cast<uint64_t>(top[1]) << 32;
        const bool roundBit = (value & 1ULL) != 0;
        value >>= 1;
        int exponent = static_cast<int>(bits) - 53;
        if (roundBit && (sticky || (value & 1ULL) != 0)) {
            ++value;
            if (value == (1ULL << 53)) {
                value >>= 1;
                ++exponent;
            }
        }
        magnitude = std::ldexp(static_cast<double>(value), exponent);
    }
    return negative_ ? -magnitude : magnitude;
}

bool BigNum::parse(std::string_view digits, int radix, bool allowSign, BigNum& out) {
    out = BigNum();
    if (radix < 2 || radix > 36) return false;
    size_t at = 0;
    bool negative = false;
    if (allowSign && at < digits.size() && (digits[at] == '+' || digits[at] == '-')) {
        negative = digits[at] == '-';
        ++at;
    }
    if (at >= digits.size()) return false;
    std::vector<uint32_t> mag;
    const RadixChunk chunk = radixChunk(radix);
    uint32_t pending = 0;
    uint32_t pendingDigits = 0;
    uint32_t pendingScale = 1;
    for (; at < digits.size(); ++at) {
        const int value = digitValue(digits[at]);
        if (value < 0 || value >= radix) return false;
        pending = pending * static_cast<uint32_t>(radix) + static_cast<uint32_t>(value);
        pendingScale *= static_cast<uint32_t>(radix);
        if (++pendingDigits == chunk.digits) {
            mulAddSmall(mag, chunk.power, pending);
            pending = 0;
            pendingDigits = 0;
            pendingScale = 1;
        }
    }
    if (pendingDigits != 0) mulAddSmall(mag, pendingScale, pending);
    out.mag_ = std::move(mag);
    out.negative_ = negative;
    out.trim();
    return true;
}

std::string BigNum::toString(int radix) const {
    if (radix < 2 || radix > 36) radix = 10;
    if (mag_.empty()) return "0";
    static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    const RadixChunk chunk = radixChunk(radix);
    std::vector<uint32_t> work = mag_;
    std::string reversed;
    while (!work.empty()) {
        uint32_t remainder = divModSmall(work, chunk.power);
        const bool last = work.empty();
        for (uint32_t i = 0; i < chunk.digits; ++i) {
            reversed.push_back(kDigits[remainder % static_cast<uint32_t>(radix)]);
            remainder /= static_cast<uint32_t>(radix);
            // The final chunk stops as soon as it runs out of value, so the
            // number has no leading zeros; every earlier chunk emits all of its
            // digits, because an interior zero is significant.
            if (last && remainder == 0) break;
        }
    }
    std::string out;
    if (negative_) out.push_back('-');
    out.append(reversed.rbegin(), reversed.rend());
    return out;
}

}  // namespace bronze::runtime
