#pragma once

// CFG mutators. Each edits the function in place and leaves every Cfg,
// DomTree, LoopForest, DefUse and Liveness built from it stale: rebuild what
// you use afterwards. New blocks are appended (a block's id is its index, and
// the entry stays b0); value ids are never reused.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "opt/core/cfg.h"
#include "opt/core/loops.h"

namespace bronze::opt {

// Puts a new block on a normal edge: `from`'s terminator targets the new
// block with the edge's arguments, and the new block, whose parameters copy
// the old target's types, jumps to the old target passing them on. Returns the
// new block, or kNoBlock for a throw edge (it leaves mid-block and carries
// nothing, so there is nowhere to put a block). The new block has no handler
// and is cold when the target is.
BlockId splitEdge(il::Function& fn, const Edge& edge);

// The loop's preheader: the existing one (LoopForest::preheader), or a new
// block every out-of-loop normal edge into the header is moved to, which jumps
// to the header passing its parameters on. kNoBlock when none can be made:
// the header is the entry block (b0 must stay the entry and take no
// parameters), or an out-of-loop throw edge enters it (a handler cannot be
// entered through another block).
BlockId ensurePreheader(il::Function& fn, const Cfg& cfg, const LoopForest& loops,
                        uint32_t loopIndex);

// Deletes every block the Cfg cannot reach from b0 (throw edges only from
// blocks that can throw, so a handler nothing throwing names goes too), and
// renumbers the rest in their old order, rewriting every target and handler.
// A handler that was deleted becomes kNoBlock on the blocks that named it,
// which can never throw to it. Answers how many blocks went.
//
// A `resume.point` inside a deleted block goes with it: code no path reaches
// is code a speculated copy cannot have left from either.
size_t removeUnreachableBlocks(il::Function& fn);

// Splits `block` in front of instruction `index` (which must be in range: the
// terminator at most): the instructions from `index` on move to a new block,
// and `block` ends in a parameterless `jump` to it. The new block keeps the
// handler and the cold flag, so a throw from a moved instruction goes where it
// went. Values need no parameters: `block` dominates the new block, whose only
// predecessor it is. The terminator moves too, so the old successors now have
// the new block as their predecessor (their edge arguments travel with it).
// Returns the new block's id.
BlockId splitBlockBefore(il::Function& fn, BlockId block, size_t index);

// Redirects the normal edge `edge` to `newTarget` with `args`, which must
// match newTarget's parameters. The old target loses that predecessor.
void retargetEdge(il::Function& fn, const Edge& edge, BlockId newTarget, std::vector<ValueId> args);

}  // namespace bronze::opt
