#pragma once

// The rules of the pass as the driver (driver.cpp) runs them. Each takes the
// function as it stands, builds the analyses it needs, edits the function and
// answers whether it changed anything. None relies on an analysis another
// built: every snapshot is rebuilt where it is used.

#include "il/il.h"
#include "opt/repr/repr.h"

namespace bronze::opt::repr {

struct PhaseContext {
    il::Module& module;
    il::Function& fn;
    ReprStats& stats;
};

// Rules 1 and 5 (fold_box.cpp): tests, unboxes, conversions and compares
// over `box` of a typed value.
bool foldBoxedUses(PhaseContext& ctx);

// Rule 2 (params_solve.cpp, params_apply.cpp): dynamic block parameters fed
// boxes of one type become parameters of that type.
bool unboxBlockParams(PhaseContext& ctx);

// Rule 4 (params_truthy.cpp): dynamic block parameters whose every use is a
// ToBoolean (`unbox.bool`) or an edge into another such become Bool.
bool truthyBlockParams(PhaseContext& ctx);

// Rule 3 (sink.cpp): boxes moved to exit blocks, or to their one use.
bool sinkBoxes(PhaseContext& ctx);

}  // namespace bronze::opt::repr
