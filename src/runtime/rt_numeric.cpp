// ECMA-262 13.15.3 ApplyStringOrNumericBinaryOperator, minus `+`, over boxed
// operands — and 13.5.4/13.5.6's unary `-` and `~` beside them.
//
// `+` is not here because it is the one operator with a THIRD algorithm (string
// concatenation) in front of the numeric ones; it keeps rt_convert.cpp's
// `bronze_dynamic_add`. What every operator below shares is the shape 13.15.3
// step 3 gives them: ToNumeric each operand, and then a hard requirement that
// the two answers have the SAME type. That requirement is the whole reason
// these helpers exist at all — before BigInt, a dynamic `a - b` was `ToNumber`
// twice and an fsub, with nothing left to decide.
//
// The number/number case is inlined at the call site (llvm_arith.cpp), so a
// call into this file means an operand was a string, an object, a BigInt or a
// Symbol. Nothing a typed numeric path emits reaches here.

#include <cmath>
#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/bigint.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// 7.1.3 ToNumeric: ToPrimitive with hint NUMBER, and then a BigInt is left
// alone where everything else becomes a Number. The BigInt survival is the
// whole of the clause — ToNumber would refuse it — and it is what lets the
// caller below ask one question, "are these the same type?", instead of
// re-deriving the answer per operand.
//
// False leaves an exception pending: a Symbol operand, or a user `valueOf`
// that threw.
bool toNumeric(Value v, Value& out) {
    // 7.1.3 step 1 on a Number is the identity, and a Number is what a
    // dynamic operand usually holds: no root, no ToPrimitive walk.
    if (v.isNumber()) {
        out = v;
        return true;
    }
    Rooted<Value> input{v};
    Rooted<Value> prim{rtToPrimitive(input, ToPrimitiveHint::Number)};
    if (rtExceptionPending()) return false;
    if (prim.get().isBigInt()) {
        out = prim.get();
        return true;
    }
    const double d = rtToNumber(prim.get());
    if (rtExceptionPending()) return false;
    out = Value::fromDouble(d);
    return true;
}

// Both operands through ToNumeric, in source order, with the first held across
// the second: `toNumeric` runs user code and allocates a BigInt, so an unrooted
// first answer would be a dangling pointer the moment the second one collects.
//
// False means an exception is pending and the caller must return undefined.
bool numericOperands(uint64_t lhsBits, uint64_t rhsBits, Rooted<Value>& lhsOut,
                     Rooted<Value>& rhsOut) {
    Rooted<Value> lhsIn{Value(lhsBits)};
    Rooted<Value> rhsIn{Value(rhsBits)};
    Value ln = Value::fromUndefined();
    if (!toNumeric(lhsIn.get(), ln)) return false;
    lhsOut.set(ln);
    Value rn = Value::fromUndefined();
    if (!toNumeric(rhsIn.get(), rn)) return false;
    rhsOut.set(rn);
    return true;
}

double asDouble(Value v) {
    return v.isInt32() ? static_cast<double>(static_cast<int32_t>(v.payload())) : v.asNumber();
}

// One arithmetic operator: the BigInt algorithm when either operand is one
// (rtBigIntBinary owns the mixing TypeError, so "either" and not "both" is the
// right test), the Number algorithm otherwise.
uint64_t arith(BigIntOp op, uint64_t lhsBits, uint64_t rhsBits) {
    Rooted<Value> l{Value::fromUndefined()};
    Rooted<Value> r{Value::fromUndefined()};
    if (!numericOperands(lhsBits, rhsBits, l, r)) return Value::fromUndefined().rawBits();
    if (l.get().isBigInt() || r.get().isBigInt()) {
        Value out = Value::fromUndefined();
        rtBigIntBinary(op, l.get(), r.get(), out);
        return out.rawBits();
    }
    const double a = asDouble(l.get());
    const double b = asDouble(r.get());
    double result = 0;
    switch (op) {
        case BigIntOp::Sub: result = a - b; break;
        case BigIntOp::Mul: result = a * b; break;
        case BigIntOp::Div: result = a / b; break;
        // Number::remainder is C's fmod — the result takes the DIVIDEND's sign
        // — which is also what the inlined frem at the call site computes.
        case BigIntOp::Mod: result = std::fmod(a, b); break;
        // The one exponentiation algorithm, shared with `Math.pow`: C's pow
        // disagrees with the language at `pow(1, NaN)` and `pow(-1, inf)`.
        default: result = rtExponentiate(a, b); break;
    }
    return Value::fromDouble(result).rawBits();
}

// One bitwise or shift operator. The Number side is ToInt32 on both operands
// (ToUint32 for the shift count's low five bits, which are the same bits), and
// the result is the JS number that int32 denotes — the same conversion ladder
// `to.int32` plus a machine op gives for a proven-numeric chain.
uint64_t bitwise(BigIntOp op, uint64_t lhsBits, uint64_t rhsBits) {
    Rooted<Value> l{Value::fromUndefined()};
    Rooted<Value> r{Value::fromUndefined()};
    if (!numericOperands(lhsBits, rhsBits, l, r)) return Value::fromUndefined().rawBits();
    if (l.get().isBigInt() || r.get().isBigInt()) {
        Value out = Value::fromUndefined();
        rtBigIntBinary(op, l.get(), r.get(), out);
        return out.rawBits();
    }
    const int32_t a = bronze_to_int32_f64(asDouble(l.get()));
    const int32_t b = bronze_to_int32_f64(asDouble(r.get()));
    const uint32_t count = static_cast<uint32_t>(b) & 31u;
    double result = 0;
    switch (op) {
        case BigIntOp::BitAnd: result = static_cast<double>(a & b); break;
        case BigIntOp::BitOr: result = static_cast<double>(a | b); break;
        case BigIntOp::BitXor: result = static_cast<double>(a ^ b); break;
        case BigIntOp::Shl:
            // Through uint32_t: a left shift that overflows a signed int32 is
            // undefined in C++ and defined (modulo 2^32) in the language.
            result = static_cast<double>(
                static_cast<int32_t>(static_cast<uint32_t>(a) << count));
            break;
        case BigIntOp::Shr: result = static_cast<double>(a >> count); break;
        // `>>>` is the one member whose result is ToUint32 rather than ToInt32:
        // `-1 >>> 0` is 4294967295.
        default: result = static_cast<double>(static_cast<uint32_t>(a) >> count); break;
    }
    return Value::fromDouble(result).rawBits();
}

}  // namespace

extern "C" {

uint64_t bronze_dynamic_sub(uint64_t l, uint64_t r) { return arith(BigIntOp::Sub, l, r); }
uint64_t bronze_dynamic_mul(uint64_t l, uint64_t r) { return arith(BigIntOp::Mul, l, r); }
uint64_t bronze_dynamic_div(uint64_t l, uint64_t r) { return arith(BigIntOp::Div, l, r); }
uint64_t bronze_dynamic_mod(uint64_t l, uint64_t r) { return arith(BigIntOp::Mod, l, r); }
uint64_t bronze_dynamic_pow(uint64_t l, uint64_t r) { return arith(BigIntOp::Pow, l, r); }

uint64_t bronze_dynamic_bitand(uint64_t l, uint64_t r) { return bitwise(BigIntOp::BitAnd, l, r); }
uint64_t bronze_dynamic_bitor(uint64_t l, uint64_t r) { return bitwise(BigIntOp::BitOr, l, r); }
uint64_t bronze_dynamic_bitxor(uint64_t l, uint64_t r) { return bitwise(BigIntOp::BitXor, l, r); }
uint64_t bronze_dynamic_shl(uint64_t l, uint64_t r) { return bitwise(BigIntOp::Shl, l, r); }
uint64_t bronze_dynamic_shr(uint64_t l, uint64_t r) { return bitwise(BigIntOp::Shr, l, r); }
uint64_t bronze_dynamic_ushr(uint64_t l, uint64_t r) { return bitwise(BigIntOp::UShr, l, r); }

// 13.5.5 unary `-`. NOT `0 - x`: IEEE-754 makes 0 - 0 positive zero, and the
// sign of zero is observable.
uint64_t bronze_dynamic_neg(uint64_t bits) {
    Value n = Value::fromUndefined();
    if (!toNumeric(Value(bits), n)) return Value::fromUndefined().rawBits();
    if (n.isBigInt()) {
        Value out = Value::fromUndefined();
        rtBigIntNegate(n, out);
        return out.rawBits();
    }
    return Value::fromDouble(-asDouble(n)).rawBits();
}

// 13.5.6 unary `~`. Its own helper rather than `x ^ -1` because that spelling
// is a MIXING error the moment `x` is a BigInt: -1 is a Number, and 13.15.3
// would refuse the pair before the operator ever ran.
uint64_t bronze_dynamic_bitnot(uint64_t bits) {
    Value n = Value::fromUndefined();
    if (!toNumeric(Value(bits), n)) return Value::fromUndefined().rawBits();
    if (n.isBigInt()) {
        Value out = Value::fromUndefined();
        rtBigIntBitNot(n, out);
        return out.rawBits();
    }
    return Value::fromDouble(static_cast<double>(~bronze_to_int32_f64(asDouble(n)))).rawBits();
}

// 7.1.3 ToNumeric on its own, which a postfix update needs because 13.4.3.1
// yields the COERCED old value: `let s = "5"; s++` evaluates to the number 5,
// not to the string.
uint64_t bronze_to_numeric(uint64_t bits) {
    Value out = Value::fromUndefined();
    if (!toNumeric(Value(bits), out)) return Value::fromUndefined().rawBits();
    return out.rawBits();
}

// 13.4.4.1 step 3: add or subtract ONE OF THE OPERAND'S OWN TYPE — 1 for a
// Number, 1n for a BigInt. Spelling it `x + 1` instead is the mixing TypeError,
// which is why `++` is an operator here rather than a rewrite in lowering.
//
// The operand has already been through ToNumeric at the call site, so it is a
// Number or a BigInt and nothing else.
uint64_t bronze_numeric_step(uint64_t bits, bool increment) {
    Value v(bits);
    if (v.isBigInt()) {
        Rooted<Value> operand{v};
        // The 1n allocates and can therefore collect, which is what the root
        // above is for.
        Rooted<Value> one{rtMakeBigIntFromInt64(1)};
        Value out = Value::fromUndefined();
        rtBigIntBinary(increment ? BigIntOp::Add : BigIntOp::Sub, operand.get(), one.get(), out);
        return out.rawBits();
    }
    return Value::fromDouble(asDouble(v) + (increment ? 1.0 : -1.0)).rawBits();
}

// A BigInt literal, from the key-pool index of its source text. Parsed rather
// than stored as bits because a literal has no width bound; a fresh object per
// evaluation is unobservable, since a BigInt is immutable and `===` compares
// its VALUE.
uint64_t bronze_bigint_literal(uint32_t keyIndex) {
    BigNum value;
    if (!rtStringToBigInt(rtKeyString(keyIndex), value)) {
        // The parser has already validated every digit, so this is a drift
        // between it and StringToBigInt rather than anything a program did.
        fatal("internal: a BigInt literal's text did not parse");
    }
    return rtMakeBigInt(value).rawBits();
}

}  // extern "C"

}  // namespace bronze::runtime
