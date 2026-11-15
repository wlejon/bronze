#pragma once

// The small pieces every rule of the pass is written with: reading a value
// back to the typed value it boxes, making the few instructions the rules
// place, and editing a block by its ORIGINAL instruction positions so a walk
// can plan edits without invalidating the indices it is walking.

#include <cstdint>
#include <optional>
#include <vector>

#include "il/il.h"
#include "opt/core/uses.h"

namespace bronze::opt::repr {

// `v` is `box.<type> %value` with %value of that very type. A box whose
// operand type disagrees with its `boxType` is not read through: nothing
// here can say what it holds.
struct BoxSource {
    il::Type type = il::Type::Void;
    ValueId value = kNoValue;
};
std::optional<BoxSource> boxSource(const il::Function& fn, const DefUse& defs, ValueId v);

// An F64 value that is EXACTLY an int32 Number (not -0): `i32.to_f64 %y`
// (then `value` is %y) or a `const.f64` that isInt32Double (then `isConst`
// and `constant`). Both are exact: i32 -> f64 loses nothing, and an int32
// never converts to -0.
struct Int32Source {
    bool isConst = false;
    int32_t constant = 0;
    ValueId value = kNoValue;
};
std::optional<Int32Source> exactInt32(const il::Function& fn, const DefUse& defs, ValueId f64);

// The double a `const.f64` value holds.
std::optional<double> constF64(const il::Function& fn, const DefUse& defs, ValueId v);

// New instructions, each with a fresh result of `fn`.
il::Instruction makeBox(il::Function& fn, ValueId x, il::Type type, const Span& span);
il::Instruction makeI32ToF64(il::Function& fn, ValueId x, const Span& span);
il::Instruction makeConstI32(il::Function& fn, int32_t value, const Span& span);

// Rewrites `inst` in place into `op` over `operands`, keeping its result id,
// result type and span, and dropping every immediate. `boxType` is the
// operand type for the ops that carry one (to.int32).
void rewriteInPlace(il::Instruction& inst, il::Op op, std::vector<ValueId> operands,
                    il::Type boxType = il::Type::Void);

// Instruction insertions and deletions for many blocks, every position
// naming an instruction as the block stood when the plan was made. `apply`
// rebuilds each edited block once: inserts in front of position `before`
// come out in the order they were added, ahead of that instruction.
class EditPlan {
public:
    void insertBefore(BlockId block, size_t before, il::Instruction inst);
    void remove(BlockId block, size_t index);
    bool empty() const { return inserts_.empty() && removes_.empty(); }
    // Answers whether any block changed.
    bool apply(il::Function& fn);

private:
    struct Insert {
        BlockId block;
        size_t before;
        size_t order;
        il::Instruction inst;
    };
    struct Remove {
        BlockId block;
        size_t index;
    };
    std::vector<Insert> inserts_;
    std::vector<Remove> removes_;
};

// Deletes every instruction whose result nothing uses and whose effects say
// it may go (il::Effects::removableIfUnused), to a fixed point. Answers how
// many went. The redundancy pass's dead code job does more; this is the part
// the rules need between them, so a dead unbox does not count as a use of the
// box the next rule wants to move.
size_t removeDeadInstructions(il::Function& fn);

}  // namespace bronze::opt::repr
