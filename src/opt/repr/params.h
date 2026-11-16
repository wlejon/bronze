#pragma once

// Rule 2's shared pieces: which typed value an edge argument stands for, the
// solution of which block parameters become typed, and its application.
//
// A DYNAMIC BLOCK PARAMETER p becomes a parameter of type T when every edge
// into its block passes, for p, a value that is a Number (or a Boolean) the
// edge's source block already holds in typed form:
//   - `box.T %x`: the typed value is %x;
//   - `box.f64 %y` with %y exactly an int32 (rewrite.h exactInt32): it is the
//     int32 too, so it serves an I32 parameter;
//   - another such parameter (optimistically, so a loop's own parameter and
//     a chain of loop parameters resolve together), or p itself around a back
//     edge;
//   - a dynamic %v for which the speculation step already made an unchecked
//     unbox in a block dominating the edge's source: that unbox ran under its
//     guard, so on this edge %v holds exactly the unbox's value. No guard is
//     invented here.
// Arguments of I32 and F64 on different edges meet at F64, the I32 ones
// converted with `i32.to_f64` (exact). A Boolean never meets a Number.

#include <cstdint>
#include <vector>

#include "il/il.h"
#include "opt/core/cfg.h"
#include "opt/core/uses.h"
#include "opt/repr/repr.h"

namespace bronze::opt::repr {

// The lattice of a candidate parameter, top (no evidence yet) to bottom (no
// single typed form).
enum class Lat : uint8_t { Top, I32, F64, Bool, Bottom };

Lat meet(Lat a, Lat b);
il::Type latType(Lat l);

// Every unchecked unbox (`unbox.i32.unchecked` / `unbox.f64.unchecked`) of
// each dynamic value, in block then instruction order.
class UnboxIndex {
public:
    UnboxIndex(const il::Function& fn, const Cfg& cfg);

    // The result of an unchecked unbox of `v` yielding `type` available at
    // the end of `from` (valueReaches; the same block counts: every
    // instruction precedes the terminator the edge leaves by), or kNoValue.
    // The first such in block order, so the answer is a function of the IL.
    ValueId available(ValueId v, il::Type type, BlockId from, const DomTree& dom) const;

private:
    struct Entry {
        BlockId block;
        il::Type type;
        ValueId result;
    };
    std::vector<std::vector<Entry>> byValue_;
    const Cfg& cfg_;
};

// One normal edge into a block: the terminator of `from`, its else-target
// when `isElse`.
struct InEdge {
    BlockId from = kNoBlock;
    bool isElse = false;
};

// The normal edges into each block, read off every block's terminator, in
// block order.
std::vector<std::vector<InEdge>> collectInEdges(const il::Function& fn);

// The arguments an InEdge passes.
const std::vector<ValueId>& inEdgeArgs(const il::Function& fn, const InEdge& e);
std::vector<ValueId>& inEdgeArgs(il::Function& fn, const InEdge& e);

struct ParamPlan {
    BlockId block = kNoBlock;
    uint32_t index = 0;       // position among the block's parameters
    ValueId id = kNoValue;    // the parameter's value id
    il::Type type = il::Type::Void;
    bool widened = false;     // F64, with an I32 argument on some edge
};

// Rule 2's solution: the parameters to retype, in block then parameter
// order. Expects every block reachable (the driver removes the others).
std::vector<ParamPlan> planParams(const il::Function& fn, const DefUse& defs, const DomTree& dom,
                                  const UnboxIndex& unboxes,
                                  const std::vector<std::vector<InEdge>>& inEdges);

// Applies a plan: retypes the parameters, boxes each once at the top of its
// block for the uses that want a Dynamic value, and gives every edge into a
// retyped parameter the typed value. `unboxes` and `dom` are those the plan
// was made with.
void applyParamPlan(il::Function& fn, const std::vector<ParamPlan>& plan, const DomTree& dom,
                    const UnboxIndex& unboxes, ReprStats& stats);

}  // namespace bronze::opt::repr
