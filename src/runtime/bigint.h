#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "runtime/bignum.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze::runtime {

// The BigInt VALUE — the bridge between bignum.h's pure arithmetic and the
// value model — and ECMA-262 6.1.6.2's operators over it.
//
// The heap layout is the string's, for the same reason: a variable-length
// payload the collector must copy and must NOT scan. `Tag::BigInt` is what
// says so (heap.cpp's `payload_holds_values`), and the limbs sit inline after
// the header exactly as a string's code units do.
//
// A BigInt is IMMUTABLE, so nothing here ever writes an existing one. That is
// what makes `===` value equality rather than identity, and what lets the
// literal cache in rt_state.cpp hand the same object to every evaluation of one
// literal without a program being able to tell.
struct BigIntHeader {
    HeapObjectHeader header;
    uint32_t limbCount;
    // 1 for a negative value. A whole word rather than a bit in `header.flags`
    // because that word is the heap KIND for a Tag::Object and a string's
    // encoding bits, and a third meaning on it would be a third thing to get
    // wrong; the padding is free either way, since the limbs must start
    // 8-aligned.
    uint32_t negative;

    const uint32_t* limbs() const noexcept {
        return reinterpret_cast<const uint32_t*>(this + 1);
    }
    uint32_t* limbs() noexcept { return reinterpret_cast<uint32_t*>(this + 1); }
};

// A heap BigInt holding `value`. ALLOCATES.
Value rtMakeBigInt(const BigNum& value);
Value rtMakeBigIntFromInt64(int64_t value);

bool rtIsBigInt(Value v) noexcept;
// The arithmetic value of a BigInt-tagged Value. A caller error for anything
// else; every caller has already asked `rtIsBigInt`.
BigNum rtBigIntValue(Value v);

// 7.1.14 StringToBigInt: the StringIntegerLiteral grammar, which is the numeric
// literal grammar MINUS the decimal point, the exponent and the `n` suffix, plus
// the leading sign a literal cannot have. The empty string (and a string of
// nothing but whitespace) is 0n; anything the whole of which is not consumed is
// UNDEFINED, which every caller turns into either a SyntaxError (`BigInt(s)`)
// or `false` (`==` against a string).
bool rtStringToBigInt(std::string_view text, BigNum& out);

// 6.1.6.2's ToString, which is also `BigInt.prototype.toString(radix)`.
std::string rtBigIntToString(Value v, int radix);

// ECMA-262 7.2.13 and 7.2.14's cross-type comparisons, exactly: no conversion
// through a double, so 9007199254740993n > 9007199254740992 is true. `-2` (the
// UNORDERED answer) for a NaN on the Number side.
int rtCompareBigIntWithNumber(Value bigintVal, double number) noexcept;

// The two-operand numeric operators of 6.1.6.2, over BOXED operands, with
// 13.15.3's mixing rule in front of every one of them: if exactly one operand
// is a BigInt the result is the TypeError "Cannot mix BigInt and other types",
// and if both are the operation is exact.
//
// `handled` is false when NEITHER operand is a BigInt, which is the signal for
// the caller to run its ordinary Number path. The split is here rather than at
// each call site because there are eleven call sites and one rule.
enum class BigIntOp {
    Add, Sub, Mul, Div, Mod, Pow,
    BitAnd, BitOr, BitXor, Shl, Shr,
    // `>>>` exists only to be refused: 6.1.6.2.11 BigInt::unsignedRightShift is
    // defined as "throw a TypeError", because an unsigned shift needs a width
    // and a BigInt has none.
    UShr,
};

// Runs the operator when either operand is a BigInt, leaving an exception
// pending for the mixing TypeError, the division-by-zero RangeError, the
// negative-exponent RangeError and `>>>`. ALLOCATES.
bool rtBigIntBinary(BigIntOp op, Value left, Value right, Value& out);

// Unary `-` and `~` for a BigInt operand; false when the operand is not one.
bool rtBigIntNegate(Value operand, Value& out);
bool rtBigIntBitNot(Value operand, Value& out);

// 6.1.6.2's ℝ -> Number, which is what `Number(bigint)` is.
double rtBigIntToNumber(Value v) noexcept;

// 25.3.1.6 RawBytesToNumeric and 25.3.1.5 NumericToRawBytes, for the two
// 64-bit BigInt rows of table 71 and nothing else. They live here rather than
// beside either caller because there are now THREE — a DataView's four 64-bit
// accessors, a BigInt64Array element, and Atomics over one — and "what does
// -1n look like in eight bytes" must have one answer.
//
// The read direction ALLOCATES (a BigInt is a heap value). The write direction
// runs 7.1.13 ToBigInt, which means it can run USER CODE for an object argument
// and can leave a TypeError pending for a Number — which is the one place a
// typed-array store throws instead of truncating. The wrap itself is
// BigInt::asUintN over 64 bits, so an out-of-range value wraps exactly as
// `setInt32` does. False means an exception is pending.
Value rtBigIntFromRawBits64(uint64_t bits, bool isSigned);
bool rtBigIntToRawBits64(Value v, uint64_t& out);

// The DECIMAL TEXT of the same eight bytes, without building a BigInt at all.
// `console.log` of a BigInt64Array needs it and must not allocate: it is walking
// a view through a raw pointer, and one heap BigInt per element would relocate
// that view under the loop.
std::string rtBigIntDecimalOfRawBits64(uint64_t bits, bool isSigned);

// 7.1.13 ToBigInt entire, step 1's ToPrimitive included — so it RUNS USER CODE
// for an object argument and the caller must root what it holds. A NUMBER is
// the TypeError the clause's table names and never a conversion: `BigInt(1)`
// converts because 21.2.1.1 routes a Number through NumberToBigInt BEFORE
// reaching here, which is a different operation with a different answer for
// `1.5`. False leaves an exception pending.
bool rtToBigInt(Value v, BigNum& out);

// The `BigInt` global (builtin_bigint.cpp): the constructor object for the
// provided-global ladder, its identity, `BigInt.prototype`, and the member
// miss check every other intrinsic prototype has.
Value rtBigIntConstructor(const std::string& name);
Value rtBigIntConstructorObject();
bool rtIsBigIntConstructor(Value fn);
Value rtBigIntPrototype();
void rtCheckBigIntProtoMember(const std::string& key);

}  // namespace bronze::runtime
