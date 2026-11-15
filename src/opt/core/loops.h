#pragma once

// Natural loops: one per header, the union of the bodies of every back edge
// into it (an edge whose target dominates its source). An irreducible cycle
// has no such edge and is no loop here; a pass that only acts on loops
// leaves it alone, which is the safe answer.

#include <cstdint>
#include <vector>

#include "opt/core/cfg.h"

namespace bronze::opt {

inline constexpr uint32_t kNoLoop = UINT32_MAX;

struct Loop {
    BlockId header = kNoBlock;
    // Every block of the loop, header included, ascending.
    std::vector<BlockId> blocks;
    // The in-loop predecessors of the header, ascending.
    std::vector<BlockId> latches;
    // The innermost enclosing loop's index, or kNoLoop.
    uint32_t parent = kNoLoop;
    // 1 for an outermost loop.
    uint32_t depth = 1;

    bool contains(BlockId b) const;
};

class LoopForest {
public:
    LoopForest(const Cfg& cfg, const DomTree& dom);

    // Every loop, each one's parent before it.
    const std::vector<Loop>& loops() const { return loops_; }
    const Loop& loop(uint32_t i) const { return loops_[i]; }
    // The innermost loop containing `b`, or kNoLoop.
    uint32_t innermost(BlockId b) const { return innermost_[b]; }
    // The loop whose header `b` is, or kNoLoop.
    uint32_t headedBy(BlockId b) const;
    // Loop nesting depth of `b`: 0 outside every loop.
    uint32_t depth(BlockId b) const;

    // The loop's PREHEADER, if it has one: the single block outside the loop
    // with an edge to the header, which is a `jump` (its only successor is
    // the header) and not a throw edge. kNoBlock otherwise; ensurePreheader
    // (cfg_edit.h) makes one.
    BlockId preheader(const Cfg& cfg, uint32_t loopIndex) const;
    // The edges that leave the loop, throw edges included.
    std::vector<Edge> exits(const Cfg& cfg, uint32_t loopIndex) const;

private:
    std::vector<Loop> loops_;
    std::vector<uint32_t> innermost_;
};

}  // namespace bronze::opt
