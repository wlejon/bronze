// The BigInt value and ECMA-262 6.1.6.2's operators over it.
//
// The rule that shapes this file is 13.15.3's, and it is a rule about what
// bronze must NOT do: a BigInt and a Number are two numeric types that do not
// convert into one another. `1n + 1` is a TypeError, not 2 and not "11" — so
// every arithmetic operator asks "is exactly one operand a BigInt?" BEFORE it
// asks anything else, and answers with the mixing error when it is. Only the
// comparisons cross the line, because 7.2.13 and 7.2.14 compare MATHEMATICAL
// values rather than converting either side (bignum.h's compareWithDouble is
// what makes that exact rather than approximate).

#include "runtime/bigint.h"

#include <cctype>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"

namespace bronze::runtime {

namespace {

// The message every mixing failure shares. One string, because a program that
// catches one of these catches all of them and the text is what it reads.
constexpr const char* kMixed = "Cannot mix BigInt and other types, use explicit conversions";

}  // namespace

Value rtMakeBigInt(const BigNum& value) {
    const std::vector<uint32_t>& limbs = value.limbs();
    const size_t payload = sizeof(uint32_t) * 2 + limbs.size() * sizeof(uint32_t);
    HeapObjectHeader* raw = rtHeap().allocate(payload, Tag::BigInt);
    auto* big = reinterpret_cast<BigIntHeader*>(raw);
    big->limbCount = static_cast<uint32_t>(limbs.size());
    big->negative = value.isNegative() ? 1u : 0u;
    for (size_t i = 0; i < limbs.size(); ++i) big->limbs()[i] = limbs[i];
    return Value::fromBigInt(big);
}

Value rtMakeBigIntFromInt64(int64_t value) { return rtMakeBigInt(BigNum::fromInt64(value)); }

bool rtIsBigInt(Value v) noexcept { return v.isBigInt(); }

BigNum rtBigIntValue(Value v) {
    if (!v.isBigInt()) fatal("internal: rtBigIntValue on a value that is not a BigInt");
    const auto* big = v.asBigInt<BigIntHeader>();
    return BigNum::fromLimbs(big->limbs(), big->limbCount, big->negative != 0);
}

double rtBigIntToNumber(Value v) noexcept { return rtBigIntValue(v).toDouble(); }

std::string rtBigIntToString(Value v, int radix) { return rtBigIntValue(v).toString(radix); }

// 7.1.14 StringToBigInt. The grammar is StringIntegerLiteral: optional
// whitespace, an optional sign on a DECIMAL form only, digits, optional
// whitespace. `0x`/`0o`/`0b` are accepted WITHOUT a sign (a NonDecimalIntegerLiteral
// has none), a legacy octal is not a form at all, and — the case programs
// actually notice — the EMPTY string is 0n, which is what makes `0n == ""` true.
bool rtStringToBigInt(std::string_view text, BigNum& out) {
    static constexpr std::string_view kSpace = " \t\n\r\f\v";
    const size_t begin = text.find_first_not_of(kSpace);
    if (begin == std::string_view::npos) {
        out = BigNum();
        return true;
    }
    const size_t end = text.find_last_not_of(kSpace) + 1;
    std::string_view body = text.substr(begin, end - begin);
    if (body.size() >= 2 && body[0] == '0') {
        int radix = 0;
        switch (body[1]) {
            case 'x': case 'X': radix = 16; break;
            case 'o': case 'O': radix = 8; break;
            case 'b': case 'B': radix = 2; break;
            default: break;
        }
        if (radix != 0) {
            return BigNum::parse(body.substr(2), radix, /*allowSign=*/false, out);
        }
    }
    return BigNum::parse(body, 10, /*allowSign=*/true, out);
}

int rtCompareBigIntWithNumber(Value bigintVal, double number) noexcept {
    return BigNum::compareWithDouble(rtBigIntValue(bigintVal), number);
}

namespace {

// The operator's own name, for the mixing diagnostic. It says WHICH operator
// refused, because `a + b * c` with one stray Number in it otherwise reports a
// failure the reader has to bisect for.
const char* opSpelling(BigIntOp op) noexcept {
    switch (op) {
        case BigIntOp::Add: return "+";
        case BigIntOp::Sub: return "-";
        case BigIntOp::Mul: return "*";
        case BigIntOp::Div: return "/";
        case BigIntOp::Mod: return "%";
        case BigIntOp::Pow: return "**";
        case BigIntOp::BitAnd: return "&";
        case BigIntOp::BitOr: return "|";
        case BigIntOp::BitXor: return "^";
        case BigIntOp::Shl: return "<<";
        case BigIntOp::Shr: return ">>";
        case BigIntOp::UShr: return ">>>";
    }
    return "?";
}

// The RangeError text for the two sizes this core refuses. Not a language
// limit — 6.1.6.2 has none — so it says what it is rather than pretending to
// quote a clause.
Value throwTooLarge() {
    return rtThrowRangeError("Maximum BigInt size exceeded");
}

bool applyError(BigNumError err) {
    switch (err) {
        case BigNumError::None: return false;
        case BigNumError::NegativeExponent:
            rtThrowRangeError("Exponent must be non-negative");
            return true;
        case BigNumError::TooLarge:
            throwTooLarge();
            return true;
        case BigNumError::DivideByZero:
            rtThrowRangeError("Division by zero");
            return true;
        case BigNumError::BadDigits:
            rtThrowError(ErrorKind::SyntaxError, "Cannot convert this string to a BigInt");
            return true;
    }
    return false;
}

}  // namespace

bool rtBigIntBinary(BigIntOp op, Value left, Value right, Value& out) {
    const bool leftBig = left.isBigInt();
    const bool rightBig = right.isBigInt();
    if (!leftBig && !rightBig) return false;
    out = Value::fromUndefined();
    // 13.15.3 step 4 / every clause of 6.1.6.2: the two operands must be the
    // SAME numeric type. Exactly one BigInt is the mixing TypeError, and it is
    // raised before any conversion — which is what keeps `1n + "1"` a string
    // concatenation (its caller never reaches here) while `1n + 1` throws.
    if (leftBig != rightBig) {
        rtThrowTypeError(std::string(kMixed) + " (the '" + opSpelling(op) +
                         "' operator needs two BigInts or two Numbers)");
        return true;
    }
    if (op == BigIntOp::UShr) {
        // 6.1.6.2.11: BigInt::unsignedRightShift is defined AS a TypeError.
        // An unsigned shift fills from a width, and a BigInt has no width for
        // the fill to come from.
        rtThrowTypeError("BigInts have no unsigned right shift, use >> instead");
        return true;
    }

    const BigNum a = rtBigIntValue(left);
    const BigNum b = rtBigIntValue(right);
    BigNumError err = BigNumError::None;
    BigNum result;
    switch (op) {
        case BigIntOp::Add: result = BigNum::add(a, b); break;
        case BigIntOp::Sub: result = BigNum::sub(a, b); break;
        case BigIntOp::Mul: result = BigNum::mul(a, b); break;
        case BigIntOp::Div:
        case BigIntOp::Mod: {
            BigNum quotient;
            BigNum remainder;
            if (!BigNum::divmod(a, b, quotient, remainder)) {
                rtThrowRangeError("Division by zero");
                return true;
            }
            result = op == BigIntOp::Div ? quotient : remainder;
            break;
        }
        case BigIntOp::Pow: result = BigNum::pow(a, b, err); break;
        case BigIntOp::BitAnd: result = BigNum::bitAnd(a, b); break;
        case BigIntOp::BitOr: result = BigNum::bitOr(a, b); break;
        case BigIntOp::BitXor: result = BigNum::bitXor(a, b); break;
        case BigIntOp::Shl: result = BigNum::shiftLeft(a, b, err); break;
        case BigIntOp::Shr: result = BigNum::shiftRight(a, b, err); break;
        case BigIntOp::UShr: break;  // answered above
    }
    if (applyError(err)) return true;
    out = rtMakeBigInt(result);
    return true;
}

bool rtBigIntNegate(Value operand, Value& out) {
    if (!operand.isBigInt()) return false;
    out = rtMakeBigInt(BigNum::negate(rtBigIntValue(operand)));
    return true;
}

bool rtBigIntBitNot(Value operand, Value& out) {
    if (!operand.isBigInt()) return false;
    out = rtMakeBigInt(BigNum::bitNot(rtBigIntValue(operand)));
    return true;
}

}  // namespace bronze::runtime
