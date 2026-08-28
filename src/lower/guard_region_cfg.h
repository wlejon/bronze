#pragma once

#include <cstdint>
#include <vector>

#include "il/il.h"

namespace bronze::lower {

// The graph facts the guarded-region pass asks a function for: who reaches
// whom, who dominates whom, which back edges there are, and what is live where.
// Separated from the policy that reads them (`guard_region_select.cpp`) and from
// the rewrite that spends them (`guard_region_build.cpp`) because these are
// facts about any CFG and those two are about this transformation.
//
// A HANDLER is an edge here. It is entered from an arbitrary point inside the
// block that names it, so counting it makes dominance weaker and liveness
// larger — both in the safe direction — and it is what makes "every edge from
// outside the region enters at the header" a true statement rather than one
// about the terminators alone.
struct Cfg {
    std::vector<std::vector<il::BlockId>> succs;
    std::vector<std::vector<il::BlockId>> preds;
    // Immediate dominators, `kNoBlock` for the entry block. Blocks unreachable
    // from the entry keep `kNoBlock` too and are never in a region: a natural
    // loop is found from a back edge, and a back edge needs dominance.
    std::vector<il::BlockId> idom;
    // Reverse postorder position of each block, or UINT32_MAX when unreachable.
    std::vector<uint32_t> rpoIndex;
    std::vector<il::BlockId> rpo;

    bool dominates(il::BlockId a, il::BlockId b) const;
};

Cfg buildCfg(const il::Function& fn);

// The natural loop of the back edge `tail -> header`: the header plus every
// block that reaches `tail` without passing through the header.
std::vector<il::BlockId> naturalLoop(const Cfg& cfg, il::BlockId header, il::BlockId tail);

// Live-in sets, one per block, as ASCENDING value ids. A block's parameters are
// definitions in that block and its target arguments are uses in the block that
// jumps, which is exactly the block-parameter form of SSA: nothing about a
// parameter is live in its own block on entry.
std::vector<std::vector<il::ValueId>> computeLiveIn(const il::Function& fn, const Cfg& cfg);

// The values live immediately BEFORE instruction `index` of block `blockId`,
// ascending. Walks back from the block's live-out, so it costs one pass over
// the block's tail rather than a second fixpoint.
std::vector<il::ValueId> liveBefore(const il::Function& fn, const Cfg& cfg,
                                    const std::vector<std::vector<il::ValueId>>& liveIn,
                                    il::BlockId blockId, uint32_t index);

// Which block defines each value, and at which instruction index. `kNoBlock`
// for a function parameter; index UINT32_MAX for a block parameter.
struct DefSite {
    il::BlockId block = il::kNoBlock;
    uint32_t index = UINT32_MAX;
};
std::vector<DefSite> computeDefSites(const il::Function& fn);

}  // namespace bronze::lower
