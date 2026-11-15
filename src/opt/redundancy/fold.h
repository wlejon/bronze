#pragma once

// CONSTANT FOLDING AND EXACT IDENTITIES (job 1): an operation whose operands
// are constants becomes the constant it computes, and an operation that
// returns one of its operands for every input becomes that operand.
//
// Only typed operations fold, with the JS-exact semantics of their type:
// I32 add/sub/mul wrap (il_op.h: they stand behind a `*.would_ovf` guard),
// F64 arithmetic is IEEE-754 as the machine does it, the bitwise family is
// int32 in and the JS Number of the int32 result out, `to.int32` is ECMA-262
// 7.1.6. A Dynamic operation never folds: it is a different algorithm
// (ToPrimitive, BigInt, strings) and calls unknown code.
//
// An identity is used only where it is exact for every value of the type:
// `x * 1` on F64 (yes, -0 * 1 is -0), `x + (-0)` on F64 but never `x + 0`
// (-0 + 0 is +0), `x - 0` on F64, `x == x` true on I32 and Bool but not on F64
// (NaN).

#include <optional>

#include "il/il.h"
#include "opt/core/uses.h"

namespace bronze::opt::redundancy {

// Reads the current definitions of values. Instructions rewritten in place
// keep their index, so a DefUse snapshot still finds them.
class ValueView {
public:
    ValueView(const il::Function& fn, const DefUse& defs) : fn_(fn), defs_(defs) {}

    const il::Instruction* def(ValueId v) const { return defs_.defInst(fn_, v); }
    const il::Instruction* defIs(ValueId v, il::Op op) const { return defs_.defInstIs(fn_, v, op); }
    il::Type type(ValueId v) const { return defs_.type(v); }

private:
    const il::Function& fn_;
    const DefUse& defs_;
};

struct FoldOutcome {
    enum class Kind : uint8_t {
        None,       // nothing to fold
        Constant,   // `inst` was rewritten in place into a constant of its type
        Identity,   // `inst`'s result equals `value`; the caller replaces it
    };
    Kind kind = Kind::None;
    ValueId value = kNoValue;
};

// Folds `inst` if it can. Rewrites it in place for Kind::Constant (same
// result id, same type, no operands); leaves it untouched otherwise.
FoldOutcome foldInstruction(il::Instruction& inst, const ValueView& values);

// Rewrites `inst` in place into a constant: `const.bool`, `const.i32` or
// `const.f64` by its type, keeping its result id and span.
void makeBoolConstant(il::Instruction& inst, bool value);
void makeI32Constant(il::Instruction& inst, int32_t value);
void makeF64Constant(il::Instruction& inst, double value);

// The constant a value holds, when its definition is one.
std::optional<bool> constBool(const ValueView& values, ValueId v);

}  // namespace bronze::opt::redundancy
