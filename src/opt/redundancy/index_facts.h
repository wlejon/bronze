#pragma once

// LOOP INDEX FACTS: what `i < n` proves about an int32 index that never goes
// below zero, for the value walk (value_walk.cpp) to fold the bounds and
// overflow tests of a counted loop.
//
//     for (let i = 0; i < a.length; i++) ... a[i] ...
//
// speculates to a header parameter i (i32, 0 on entry, `add i, 1` round the
// back edge behind `add.would_ovf i, 1`), the signed `cmp.lt i, n`, and per
// iteration `in.bounds i, n` in front of the element access and the overflow
// test of the increment. Once i >= 0 is known, the true edge of `i < n`
// answers both: 0 <= i < n is the unsigned compare `in.bounds` makes, and
// i + 1 <= n <= INT32_MAX is a sum that cannot overflow.
//
// NON-NEGATIVE VALUES. An i32 value is non-negative when every value it can
// hold is >= 0, proven by induction over the run: its definition makes a
// non-negative value from operands that are non-negative when it runs. The
// definitions that do:
//   - `const.i32` k with k >= 0;
//   - an i32 `add` of two non-negative values that cannot wrap: it stands
//     where a branch on `add.would_ovf` of the same operands answered false
//     (its false edge dominates the add), or it adds a constant 0 or 1 to x
//     where an `x < n` edge (below) dominates it: x < n <= INT32_MAX, so
//     x + 1 is an int32;
//   - a block parameter every incoming edge passes a non-negative value (a
//     parameter a throw edge can enter is not one: the edge passes nothing
//     this sees).
// Everything else (lengths included: a length word is not read as proven
// non-negative here) is not. The set is the greatest fixed point: every
// candidate starts in, and one whose definition does not hold given the
// rest is dropped, until nothing changes. That is the induction: the entry
// value of a counter is >= 0, and an iteration that starts with i >= 0 ends
// with i + 1 >= 0 when the add cannot wrap.
//
// LESS-THAN EDGES. A block entered by one edge only, a branch arm on an i32
// compare, knows x < n on that arm: the true arm of `cmp.lt x, n` or
// `cmp.gt n, x`, the false arm of `cmp.ge x, n` or `cmp.le n, x` (integer
// compares, where the negation is exact). With x non-negative it adds
// InBounds(x, n) and InRange(x, n, 0..0) (edge_facts.h) on entry: the first
// answers `in.bounds x, n`, the second (tests.cpp addOverflowTest) the
// overflow test of x + 1.
//
// The analysis is of the function as it stands when the walk starts; the
// walk rewrites in place, and a fact it proves holds of the values, not of
// the tests that proved it, so folding the tests it answers leaves it true.

#include <vector>

#include "il/il.h"
#include "opt/core/cfg.h"
#include "opt/core/edge_facts.h"
#include "opt/core/uses.h"

namespace bronze::opt::redundancy {

class IndexFacts {
public:
    // Analyses `fn` (a speculated function; for any other the answers are
    // empty). `dom` may be a forest.
    IndexFacts(const il::Function& fn, const Cfg& cfg, const DomTree& dom, const DefUse& defs);

    // Whether every value `v` can hold is >= 0.
    bool nonNegative(ValueId v) const { return v < nonNeg_.size() && nonNeg_[v] != 0; }
    // The facts that hold on entry to `b` because of its less-than edge.
    std::vector<Fact> onEntry(BlockId b) const;

private:
    // x < n holds on entry to `block` and in what it dominates.
    struct LessThan {
        BlockId block = kNoBlock;
        ValueId x = kNoValue;
        ValueId n = kNoValue;
    };
    bool lessThanDominates(BlockId b, ValueId x) const;
    bool addCannotWrap(BlockId b, const il::Instruction& add) const;

    const il::Function& fn_;
    const Cfg& cfg_;
    const DomTree& dom_;
    const DefUse& defs_;
    std::vector<uint8_t> nonNeg_;
    std::vector<LessThan> lessThan_;      // by block, ascending
    std::vector<int32_t> lessThanOf_;     // block -> index into lessThan_, -1
};

}  // namespace bronze::opt::redundancy
