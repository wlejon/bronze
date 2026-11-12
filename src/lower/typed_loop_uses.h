#pragma once

// Uses and number sources of IL values, for the typed-loop versioning pass
// (typed_loop_version.h).

#include <cstdint>
#include <vector>

#include "il/il.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

// One use of a value: the instruction it is in, and whether it is an operand
// or an argument of one of the instruction's block targets.
struct Use {
    il::BlockId block = il::kNoBlock;
    uint32_t index = 0;
    enum Where : uint8_t { Operand, ThenArg, ElseArg } where = Operand;
    uint32_t slot = 0;  // which operand
};

// Every value's uses, indexed by value.
std::vector<std::vector<Use>> computeUses(const il::Function& fn);

// The f64 or i32 a dynamic index or value was boxed from, or kNoValue.
il::ValueId numberSource(const il::Function& fn, const std::vector<DefSite>& defs, il::ValueId v, bool allowI32);

// An instruction that reads an f64 operand and answers the same for NaN as the
// language answers for `undefined` in that position: each of these applies
// ToNumber (or ToNumeric, whose answer for `undefined` is the same NaN) to it,
// and the other operand of a binary one is an f64 too, so no string or BigInt
// branch is reachable. `slot` matters only for the typed store, whose index is
// not such a position: a property key `"undefined"` is a named lookup on the
// view's chain, and `"NaN"` is an index that misses.
bool isCoercingF64Use(const il::Instruction& inst, uint32_t slot);

}  // namespace bronze::lower
