#pragma once

// REPRESENTATION SELECTION: keeps the numbers and booleans of a speculated
// function S in machine form between the sites that use them.
//
// The speculation step rewrites each arithmetic site of S into guards, an
// unchecked unbox, the typed operation and a `box` of the typed result, so
// that the rest of S is unchanged. What it leaves is a value that is a number
// travelling boxed: through block parameters around loops, through `box`
// then `is.i32` + `unbox` at the next site, into the state of every exit.
// This pass removes that traffic:
//
//   1. a tag test, unchecked unbox or conversion whose operand is the `box`
//      of a typed value is answered from the typed value (fold_box.cpp);
//      the same for a `box.bool` reaching ToBoolean and for compares of two
//      boxed numbers (rule 5);
//   2. a dynamic block parameter every edge feeds a box of the same type
//      (around a loop: optimistically, the parameter itself or another such
//      parameter) becomes a parameter of that type; I32 and F64 meet at F64
//      (params_solve.cpp, params_apply.cpp);
//   3. a `box` whose every use is in blocks that only lead to a `leave`, or
//      whose one use is in a block it strictly dominates in the same loops,
//      is moved to its uses (sink.cpp).
// Stores, calls, returns and `leave` states still take the boxed value: a
// box right in front of them is the minimum without typed slots or a typed
// calling convention.
//
// What the encoding of a box is (which tests it passes) is decided in ONE
// place, encoding.h.
//
// Generic functions are left alone: they hold no unchecked forms, and the
// representation of their values is what their resume points promise.
//
// Deterministic: every walk is in block and instruction order, and no
// unordered container decides a result.

#include <cstdint>

#include "il/il.h"
#include "opt/redundancy/redundancy.h"

namespace bronze::opt {

struct ReprStats {
    uint64_t rounds = 0;
    // Rule 1 and 5.
    uint64_t testsFolded = 0;        // a tag test answered by the box's type
    uint64_t unboxesFolded = 0;      // an unchecked unbox replaced by the typed value
    uint64_t conversionsFolded = 0;  // to.numeric, to.int32, checked unbox, ToBoolean
    uint64_t comparesTyped = 0;      // strict.eq / loose.eq / rel.* of two boxes
    // Rule 2.
    uint64_t paramsUnboxed = 0;      // dynamic block parameters given a typed type
    uint64_t paramsWidened = 0;      // of those, F64 parameters some edge feeds an i32
    uint64_t edgeConversions = 0;    // i32.to_f64 / const.i32 placed on an edge's source
    // Rule 4.
    uint64_t paramsTruthy = 0;       // dynamic block parameters only tested, made Bool
    // Rule 3.
    uint64_t boxesSunkToExits = 0;   // boxes whose every use was in exit blocks
    uint64_t exitBoxClones = 0;      // the copies placed in those exit blocks
    uint64_t boxesSunk = 0;          // single-use boxes moved to their use
    uint64_t deadRemoved = 0;        // unused removable instructions deleted
    // The redundancy pass the module driver runs afterwards.
    RedundancyStats redundancy;

    void add(const ReprStats& o);
};

// Runs the pass on one function. A function that is not speculated, is
// external or has no blocks is left alone (answers false). Answers whether it
// changed anything. Leaves the branch cleanup to the redundancy pass: a test
// folded to a constant stays a `br` on that constant until it runs.
bool selectRepresentations(il::Module& module, il::Function& fn, ReprStats* stats = nullptr);

// Runs the pass on every speculated function, then the redundancy pass over
// the whole module (runRedundancyModule with `cleanup`): it turns the guards
// whose tests this folded into jumps, deletes their exit blocks and the
// resume points of G no `leave` names any more, and value-numbers the boxes
// this placed. Returns the totals.
ReprStats selectRepresentationsModule(il::Module& module, const RedundancyOptions& cleanup = {});

}  // namespace bronze::opt
