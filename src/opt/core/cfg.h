#pragma once

// The control-flow graph of one IL function, and its dominator tree.
//
// Both are SNAPSHOTS: built from the function as it stands and never updated
// by the mutators in cfg_edit.h, which say what they invalidate. A pass that
// edits the CFG rebuilds them; they are cheap (linear, and the dominator
// iteration converges in two or three passes over a reducible function).
//
// The THROW EDGE: a block whose instructions can throw (il/effects.h, `throws`,
// the `throw` terminator included) and that names a handler has an edge to
// it. It is an edge like the others for reachability, dominance, loops and
// liveness, but it leaves from the middle of the block, carries no arguments
// and cannot be split. A block that cannot throw has no edge to its handler,
// so a handler only such blocks name is unreachable.

#include <cstdint>
#include <optional>
#include <vector>

#include "il/il.h"

namespace bronze::opt {

using il::BlockId;
using il::kNoBlock;
using il::kNoValue;
using il::ValueId;

enum class EdgeKind : uint8_t {
    Jump,        // `jump`'s target
    BranchThen,  // `br`'s then-target
    BranchElse,  // `br`'s else-target
    Throw,       // the block's handler, from a throwing instruction
};

struct Edge {
    BlockId from = kNoBlock;
    BlockId to = kNoBlock;
    EdgeKind kind = EdgeKind::Jump;
};

// Whether any instruction of `block` can throw, `throw` included.
bool blockCanThrow(const il::Block& block);

// The arguments an edge passes (empty for a throw edge). `fn` must be the
// function the edge was read from.
const std::vector<ValueId>& edgeArgs(const il::Function& fn, const Edge& edge);

// The mutable BlockTarget a normal edge is written in; nullptr for a throw
// edge.
il::BlockTarget* edgeTarget(il::Function& fn, const Edge& edge);

class Cfg {
public:
    static constexpr uint32_t kUnreached = UINT32_MAX;

    explicit Cfg(const il::Function& fn);

    size_t size() const { return succs_.size(); }

    // Distinct successor / predecessor blocks, throw edges included, in a
    // fixed order (terminator targets first, then the handler).
    const std::vector<BlockId>& succs(BlockId b) const { return succs_[b]; }
    const std::vector<BlockId>& preds(BlockId b) const { return preds_[b]; }
    // Every edge, one per (terminator target or handler): a block reached by
    // both a branch and a throw edge from one predecessor has two.
    const std::vector<Edge>& outEdges(BlockId b) const { return out_[b]; }
    const std::vector<Edge>& inEdges(BlockId b) const { return in_[b]; }
    // The one edge into `b`, when there is exactly one.
    std::optional<Edge> uniqueInEdge(BlockId b) const;
    // The handler `b` throws to, or kNoBlock (no handler, or nothing in `b`
    // can throw).
    BlockId throwSucc(BlockId b) const { return throwSucc_[b]; }

    // Reverse postorder from the entry; only reachable blocks appear.
    const std::vector<BlockId>& rpo() const { return rpo_; }
    uint32_t rpoIndex(BlockId b) const { return rpoIndex_[b]; }
    bool reachable(BlockId b) const { return rpoIndex_[b] != kUnreached; }
    // Postorder (the reverse of `rpo`), for backward problems.
    std::vector<BlockId> postorder() const { return {rpo_.rbegin(), rpo_.rend()}; }

private:
    std::vector<std::vector<BlockId>> succs_;
    std::vector<std::vector<BlockId>> preds_;
    std::vector<std::vector<Edge>> out_;
    std::vector<std::vector<Edge>> in_;
    std::vector<BlockId> throwSucc_;
    std::vector<BlockId> rpo_;
    std::vector<uint32_t> rpoIndex_;
};

// Cooper, Harvey and Kennedy's iterative dominators over a Cfg's reachable
// blocks, with the tree's pre/post numbering for O(1) dominance queries.
class DomTree {
public:
    explicit DomTree(const Cfg& cfg);
    // The dominators of the graph in which each block of `extraEntries` is
    // also entered from outside the function, as the block holding a
    // `resume.point` of G is by a resumption: a FOREST, rooted at the entry
    // and at each extra entry (idom kNoBlock), and a block dominates another
    // only within one tree. `commonDominator` is not meaningful across trees.
    DomTree(const Cfg& cfg, const std::vector<BlockId>& extraEntries);

    // The entry, then each extra entry that is a root, ascending.
    const std::vector<BlockId>& roots() const { return roots_; }

    // The immediate dominator; kNoBlock for the entry and unreachable blocks.
    BlockId idom(BlockId b) const { return idom_[b]; }
    // Children in ascending block order.
    const std::vector<BlockId>& children(BlockId b) const { return children_[b]; }
    // Whether every path from the entry to `b` passes `a` (a == b counts). A
    // block no path reaches is dominated by every block, vacuously, as the
    // verifier has it; an unreachable `a` dominates only itself.
    bool dominates(BlockId a, BlockId b) const;
    bool strictlyDominates(BlockId a, BlockId b) const { return a != b && dominates(a, b); }
    // Depth in the tree (entry 0); 0 for an unreachable block.
    uint32_t depth(BlockId b) const { return depth_[b]; }
    // Reachable blocks in tree preorder, children ascending: every block
    // after its dominators.
    const std::vector<BlockId>& preorder() const { return preorder_; }
    // The nearest common dominator of two reachable blocks.
    BlockId commonDominator(BlockId a, BlockId b) const;

private:
    std::vector<BlockId> idom_;
    std::vector<std::vector<BlockId>> children_;
    std::vector<uint32_t> depth_;
    std::vector<uint32_t> pre_;
    std::vector<uint32_t> post_;
    std::vector<BlockId> preorder_;
    std::vector<uint8_t> reachable_;
    std::vector<BlockId> roots_;
};

}  // namespace bronze::opt
