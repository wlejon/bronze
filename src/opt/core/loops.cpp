#include "opt/core/loops.h"

#include <algorithm>
#include <map>

namespace bronze::opt {

bool Loop::contains(BlockId b) const { return std::binary_search(blocks.begin(), blocks.end(), b); }

LoopForest::LoopForest(const Cfg& cfg, const DomTree& dom) {
    const size_t n = cfg.size();
    innermost_.assign(n, kNoLoop);

    // Back edges grouped by header, headers ascending so the result is a
    // function of the IL alone.
    std::map<BlockId, std::vector<BlockId>> latchesOf;
    for (BlockId b : cfg.rpo()) {
        for (BlockId s : cfg.succs(b)) {
            if (cfg.reachable(s) && dom.dominates(s, b)) latchesOf[s].push_back(b);
        }
    }

    std::vector<Loop> found;
    std::vector<uint8_t> inLoop(n, 0);
    for (auto& [header, latches] : latchesOf) {
        Loop loop;
        loop.header = header;
        std::sort(latches.begin(), latches.end());
        latches.erase(std::unique(latches.begin(), latches.end()), latches.end());
        loop.latches = latches;
        // The body: everything that reaches a latch backwards without passing
        // the header. Every such block is dominated by the header, so the walk
        // never leaves the loop.
        std::fill(inLoop.begin(), inLoop.end(), uint8_t{0});
        inLoop[header] = 1;
        std::vector<BlockId> work;
        for (BlockId l : latches) {
            if (!inLoop[l]) {
                inLoop[l] = 1;
                work.push_back(l);
            }
        }
        while (!work.empty()) {
            const BlockId b = work.back();
            work.pop_back();
            for (BlockId p : cfg.preds(b)) {
                if (!inLoop[p] && cfg.reachable(p)) {
                    inLoop[p] = 1;
                    work.push_back(p);
                }
            }
        }
        for (size_t b = 0; b < n; ++b) {
            if (inLoop[b]) loop.blocks.push_back(static_cast<BlockId>(b));
        }
        found.push_back(std::move(loop));
    }

    // Outer loops first: a loop containing another is strictly larger, and
    // two loops of one size are disjoint. Ties broken by header for a fixed
    // order.
    std::stable_sort(found.begin(), found.end(), [](const Loop& a, const Loop& b) {
        if (a.blocks.size() != b.blocks.size()) return a.blocks.size() > b.blocks.size();
        return a.header < b.header;
    });
    loops_ = std::move(found);
    for (uint32_t i = 0; i < loops_.size(); ++i) {
        Loop& loop = loops_[i];
        // Every larger loop is already placed, so whatever claims the header
        // now is the smallest loop around this one.
        loop.parent = innermost_[loop.header];
        loop.depth = loop.parent == kNoLoop ? 1 : loops_[loop.parent].depth + 1;
        for (BlockId b : loop.blocks) innermost_[b] = i;
    }
}

uint32_t LoopForest::headedBy(BlockId b) const {
    const uint32_t i = innermost_[b];
    return i != kNoLoop && loops_[i].header == b ? i : kNoLoop;
}

uint32_t LoopForest::depth(BlockId b) const {
    const uint32_t i = innermost_[b];
    return i == kNoLoop ? 0 : loops_[i].depth;
}

BlockId LoopForest::preheader(const Cfg& cfg, uint32_t loopIndex) const {
    const Loop& loop = loops_[loopIndex];
    BlockId candidate = kNoBlock;
    for (const Edge& e : cfg.inEdges(loop.header)) {
        if (loop.contains(e.from)) continue;
        if (candidate != kNoBlock || e.kind != EdgeKind::Jump) return kNoBlock;
        candidate = e.from;
    }
    if (candidate == kNoBlock) return kNoBlock;
    // A jump has one target, but the block may also throw to the header.
    return cfg.outEdges(candidate).size() == 1 ? candidate : kNoBlock;
}

std::vector<Edge> LoopForest::exits(const Cfg& cfg, uint32_t loopIndex) const {
    const Loop& loop = loops_[loopIndex];
    std::vector<Edge> out;
    for (BlockId b : loop.blocks) {
        for (const Edge& e : cfg.outEdges(b)) {
            if (!loop.contains(e.to)) out.push_back(e);
        }
    }
    return out;
}

}  // namespace bronze::opt
