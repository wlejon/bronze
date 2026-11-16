#include "opt/redundancy/dom_walk.h"

#include "opt/core/edge_facts.h"

namespace bronze::opt::redundancy {

void walkDominatorTree(const il::Function& fn, const Cfg& cfg, const DomTree& dom, const DefUse& defs,
                       const AliasInfo& alias, FactEnv& facts, ExprTable& exprs,
                       const std::function<void(BlockId)>& visit,
                       const std::function<bool(BlockId, BlockId)>& deadEdge) {
    if (fn.blocks.empty()) return;
    // Taken before the walk: in-place rewrites only ever remove writes, so the
    // summaries stay a superset of what the blocks do.
    const std::vector<WriteSet> writes = blockWriteSets(fn);
    struct Frame {
        BlockId block;
        bool leaving;
    };
    std::vector<Frame> stack;
    for (auto it = dom.roots().rbegin(); it != dom.roots().rend(); ++it) stack.push_back({*it, false});
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        if (frame.leaving) {
            exprs.pop();
            facts.pop();
            continue;
        }
        const BlockId b = frame.block;
        facts.push();
        exprs.push();
        stack.push_back({b, true});
        // A root (the entry, or a block a resumption enters) starts with
        // nothing known.
        if (dom.idom(b) != kNoBlock) {
            if (cfg.inEdges(b).size() == 1) {
                facts.addAll(entryFacts(fn, cfg, defs, b));
            } else {
                const WriteSet between = regionWrites(cfg, writes, dom.idom(b), b, &deadEdge);
                facts.killLocs(between.allLocs());
                exprs.killAll(between, alias);
            }
            // Entered, on some path, through the throw edge of the immediate
            // dominator: what that block computed may not exist here.
            if (cfg.throwReaches(dom.idom(b), b)) exprs.hideAll();
        }
        visit(b);
        const auto& kids = dom.children(b);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back({*it, false});
    }
}

}  // namespace bronze::opt::redundancy
