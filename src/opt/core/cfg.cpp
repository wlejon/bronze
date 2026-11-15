#include "opt/core/cfg.h"

#include <algorithm>

#include "il/effects.h"

namespace bronze::opt {

bool blockCanThrow(const il::Block& block) {
    for (const il::Instruction& inst : block.instructions) {
        if (il::effectsOf(inst).throws) return true;
    }
    return false;
}

const std::vector<ValueId>& edgeArgs(const il::Function& fn, const Edge& edge) {
    static const std::vector<ValueId> kNone;
    if (edge.kind == EdgeKind::Throw) return kNone;
    const il::Instruction& term = fn.blocks[edge.from].instructions.back();
    return edge.kind == EdgeKind::BranchElse ? term.elseTarget.args : term.target.args;
}

il::BlockTarget* edgeTarget(il::Function& fn, const Edge& edge) {
    if (edge.kind == EdgeKind::Throw) return nullptr;
    il::Instruction& term = fn.blocks[edge.from].instructions.back();
    return edge.kind == EdgeKind::BranchElse ? &term.elseTarget : &term.target;
}

Cfg::Cfg(const il::Function& fn) {
    const size_t n = fn.blocks.size();
    succs_.resize(n);
    preds_.resize(n);
    out_.resize(n);
    in_.resize(n);
    throwSucc_.assign(n, kNoBlock);
    rpoIndex_.assign(n, kUnreached);

    auto addEdge = [&](BlockId from, BlockId to, EdgeKind kind) {
        if (to == kNoBlock || to >= n) return;
        const Edge e{from, to, kind};
        out_[from].push_back(e);
        in_[to].push_back(e);
        auto& s = succs_[from];
        if (std::find(s.begin(), s.end(), to) == s.end()) s.push_back(to);
        auto& p = preds_[to];
        if (std::find(p.begin(), p.end(), from) == p.end()) p.push_back(from);
    };

    for (size_t b = 0; b < n; ++b) {
        const il::Block& block = fn.blocks[b];
        const BlockId id = static_cast<BlockId>(b);
        if (!block.instructions.empty()) {
            const il::Instruction& term = block.instructions.back();
            if (term.op == il::Op::Jump) {
                addEdge(id, term.target.block, EdgeKind::Jump);
            } else if (term.op == il::Op::Branch) {
                addEdge(id, term.target.block, EdgeKind::BranchThen);
                addEdge(id, term.elseTarget.block, EdgeKind::BranchElse);
            }
        }
        if (block.handler != kNoBlock && block.handler < n && blockCanThrow(block)) {
            throwSucc_[b] = block.handler;
            addEdge(id, block.handler, EdgeKind::Throw);
        }
    }

    if (n == 0) return;
    // Iterative DFS; successors in their fixed order so the numbering is a
    // function of the IL alone.
    std::vector<uint8_t> seen(n, 0);
    std::vector<BlockId> post;
    post.reserve(n);
    std::vector<std::pair<BlockId, size_t>> stack;
    seen[0] = 1;
    stack.push_back({0, 0});
    while (!stack.empty()) {
        auto& [b, next] = stack.back();
        if (next < succs_[b].size()) {
            const BlockId s = succs_[b][next++];
            if (!seen[s]) {
                seen[s] = 1;
                stack.push_back({s, 0});
            }
            continue;
        }
        post.push_back(b);
        stack.pop_back();
    }
    rpo_.assign(post.rbegin(), post.rend());
    for (uint32_t i = 0; i < rpo_.size(); ++i) rpoIndex_[rpo_[i]] = i;
}

std::optional<Edge> Cfg::uniqueInEdge(BlockId b) const {
    if (in_[b].size() != 1) return std::nullopt;
    return in_[b].front();
}

DomTree::DomTree(const Cfg& cfg) {
    const size_t n = cfg.size();
    idom_.assign(n, kNoBlock);
    children_.resize(n);
    depth_.assign(n, 0);
    pre_.assign(n, 0);
    post_.assign(n, 0);
    reachable_.assign(n, 0);
    if (n == 0) return;
    for (BlockId b : cfg.rpo()) reachable_[b] = 1;

    // Cooper-Harvey-Kennedy, with the entry as its own idom during the
    // iteration so `intersect` terminates there.
    const auto& rpo = cfg.rpo();
    idom_[0] = 0;
    auto intersect = [&](BlockId a, BlockId b) {
        while (a != b) {
            while (cfg.rpoIndex(a) > cfg.rpoIndex(b)) a = idom_[a];
            while (cfg.rpoIndex(b) > cfg.rpoIndex(a)) b = idom_[b];
        }
        return a;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 1; i < rpo.size(); ++i) {
            const BlockId b = rpo[i];
            BlockId next = kNoBlock;
            for (BlockId p : cfg.preds(b)) {
                if (!cfg.reachable(p) || idom_[p] == kNoBlock) continue;
                next = next == kNoBlock ? p : intersect(next, p);
            }
            if (next != kNoBlock && idom_[b] != next) {
                idom_[b] = next;
                changed = true;
            }
        }
    }
    idom_[0] = kNoBlock;

    for (size_t b = 1; b < n; ++b) {
        if (idom_[b] != kNoBlock) children_[idom_[b]].push_back(static_cast<BlockId>(b));
    }
    // Children are pushed in ascending order already; the preorder walk below
    // visits them in that order.
    uint32_t clock = 0;
    std::vector<std::pair<BlockId, size_t>> stack;
    stack.push_back({0, 0});
    pre_[0] = clock++;
    preorder_.push_back(0);
    while (!stack.empty()) {
        auto& [b, next] = stack.back();
        if (next < children_[b].size()) {
            const BlockId c = children_[b][next++];
            depth_[c] = depth_[b] + 1;
            pre_[c] = clock++;
            preorder_.push_back(c);
            stack.push_back({c, 0});
            continue;
        }
        post_[b] = clock++;
        stack.pop_back();
    }
}

bool DomTree::dominates(BlockId a, BlockId b) const {
    if (a == b) return true;
    if (!reachable_[b]) return true;
    if (!reachable_[a]) return false;
    return pre_[a] <= pre_[b] && post_[b] <= post_[a];
}

BlockId DomTree::commonDominator(BlockId a, BlockId b) const {
    while (a != b) {
        if (depth_[a] > depth_[b]) {
            a = idom_[a];
        } else if (depth_[b] > depth_[a]) {
            b = idom_[b];
        } else {
            a = idom_[a];
            b = idom_[b];
        }
        if (a == kNoBlock || b == kNoBlock) return 0;
    }
    return a;
}

}  // namespace bronze::opt
