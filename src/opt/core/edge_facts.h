#pragma once

// What a branch edge proves.
//
// A speculated function's guards are ordinary `br`s, so everything it knows
// it knows from edges: the true edge of `br (is.i32 %x)` proves %x holds an
// int32, the false edge that it does not. `branchFacts` reads a `br`'s
// condition back to the test that made it and answers the facts each edge
// carries; fact_env.h holds them over a dominator-tree walk.
//
// A fact about a VALUE (its tag, which function it is, `i < len` between two
// SSA values) holds for as long as the value does. A fact about an object's
// MEMORY (its shape, its array kind) holds only until something writes that
// memory, so it carries the LocSet it depends on and dies at the first
// instruction whose effects write any of it (FactEnv::killBy). Such a fact is
// only derived when its test sits in the branch's own block with no write of
// those locations between the test and the branch; otherwise the edge proves
// it about a moment that has passed.

#include <cstdint>
#include <optional>
#include <vector>

#include "il/effects.h"
#include "il/il.h"
#include "opt/core/cfg.h"
#include "opt/core/uses.h"

namespace bronze::opt {

enum class FactKind : uint8_t {
    BoolValue,   // value (bool) is `imm` (0 or 1)
    I32,         // value is a Number holding an int32 (`is.i32`)
    Number,      // value is in the double box (`is.number`)
    Object,      // value is a heap object (`is.object`)
    Nullish,     // value is null or undefined (`is.nullish`)
    Hole,        // value is the array hole (`is.hole`)
    Shape,       // value has the shape of layout cell `imm`; depends on Shapes
    DenseArray,  // value passes `is.dense_array value, imm`; depends on Shapes, ArrayLen, Elements
    PlainArray,  // value passes `is.plain_array`; depends on Shapes
    TypedArray,  // value passes `is.typed_array value, imm`; with the pristine bit depends on Shapes, Heap
    Array,       // value is an Array (`is.array`): about the value, it never ends
    Callee,      // value is a function whose code is module function `imm` (`fn.code_is`)
    InBounds,    // (uint32)value < (uint32)other (`in.bounds`)
    PtrEq,       // ptr value == ptr other (a `ptr.eq` that is not a shape test)
};

const char* factKindName(FactKind kind);

struct Fact {
    FactKind kind = FactKind::BoolValue;
    // False: the NEGATION is what is known (the false edge of a test).
    bool holds = true;
    ValueId value = kNoValue;
    ValueId other = kNoValue;  // InBounds: the length; PtrEq: the second pointer
    int64_t imm = 0;
    // The locations whose write kills this fact; empty for a fact about a value.
    il::LocSet dependsOn;

    bool operator==(const Fact&) const = default;
};

struct EdgeFacts {
    std::vector<Fact> onTrue;   // hold on the `br`'s then-edge
    std::vector<Fact> onFalse;  // hold on its else-edge
};

// The facts each edge of block `b`'s terminating `br` proves. Empty when the
// block does not end in a `br`. Always includes the condition's own value
// (BoolValue) on each edge.
EdgeFacts branchFacts(const il::Function& fn, const DefUse& defs, BlockId b);

// The facts that hold on ENTRY to `b` because of the edge into it: the
// branch facts of that edge when `b` has exactly one incoming edge and it is
// a branch arm; nothing otherwise (a join, a jump, a throw edge, the entry).
std::vector<Fact> entryFacts(const il::Function& fn, const Cfg& cfg, const DefUse& defs, BlockId b);

}  // namespace bronze::opt
