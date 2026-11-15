#include "opt/core/cfg_edit.h"

#include <iterator>

#include "opt/core/uses.h"

namespace bronze::opt {

namespace {

// A new block whose fresh parameters copy `target`'s and which jumps there,
// passing them on. Returns its id.
BlockId makeForwarder(il::Function& fn, BlockId target) {
    // Copy what is needed from the target first: newBlock reallocates.
    std::vector<il::Type> types;
    for (const il::BlockParam& p : fn.blocks[target].params) types.push_back(p.type);
    const bool cold = fn.blocks[target].cold;

    il::Block& block = newBlock(fn);
    const BlockId id = block.id;
    block.cold = cold;
    il::Instruction jump;
    jump.op = il::Op::Jump;
    jump.target.block = target;
    for (il::Type t : types) {
        const ValueId v = newValue(fn);
        fn.blocks[id].params.push_back(il::BlockParam{v, t});
        jump.target.args.push_back(v);
    }
    fn.blocks[id].instructions.push_back(std::move(jump));
    return id;
}

}  // namespace

void retargetEdge(il::Function& fn, const Edge& edge, BlockId newTarget, std::vector<ValueId> args) {
    il::BlockTarget* t = edgeTarget(fn, edge);
    if (t == nullptr) return;
    t->block = newTarget;
    t->args = std::move(args);
}

BlockId splitBlockBefore(il::Function& fn, BlockId block, size_t index) {
    // newBlock reallocates the block vector: take what is moved first.
    std::vector<il::Instruction>& from = fn.blocks[block].instructions;
    std::vector<il::Instruction> moved(std::make_move_iterator(from.begin() + static_cast<std::ptrdiff_t>(index)),
                                       std::make_move_iterator(from.end()));
    from.erase(from.begin() + static_cast<std::ptrdiff_t>(index), from.end());
    const BlockId handler = fn.blocks[block].handler;
    const bool cold = fn.blocks[block].cold;
    const Span span = moved.empty() ? Span{} : moved.front().span;

    il::Block& tail = newBlock(fn);
    const BlockId tailId = tail.id;
    tail.handler = handler;
    tail.cold = cold;
    tail.instructions = std::move(moved);

    il::Instruction jump;
    jump.op = il::Op::Jump;
    jump.target.block = tailId;
    jump.span = span;
    fn.blocks[block].instructions.push_back(std::move(jump));
    return tailId;
}

BlockId splitEdge(il::Function& fn, const Edge& edge) {
    if (edge.kind == EdgeKind::Throw) return kNoBlock;
    const BlockId mid = makeForwarder(fn, edge.to);
    // The edge keeps its arguments; only where they land moves.
    if (il::BlockTarget* t = edgeTarget(fn, edge)) t->block = mid;
    return mid;
}

BlockId ensurePreheader(il::Function& fn, const Cfg& cfg, const LoopForest& loops,
                        uint32_t loopIndex) {
    const BlockId existing = loops.preheader(cfg, loopIndex);
    if (existing != kNoBlock) return existing;
    const Loop& loop = loops.loop(loopIndex);
    if (loop.header == 0) return kNoBlock;
    std::vector<Edge> entries;
    for (const Edge& e : cfg.inEdges(loop.header)) {
        if (loop.contains(e.from)) continue;
        if (e.kind == EdgeKind::Throw) return kNoBlock;
        entries.push_back(e);
    }
    if (entries.empty()) return kNoBlock;
    const BlockId pre = makeForwarder(fn, loop.header);
    for (const Edge& e : entries) {
        if (il::BlockTarget* t = edgeTarget(fn, e)) t->block = pre;
    }
    return pre;
}

size_t removeUnreachableBlocks(il::Function& fn) {
    if (fn.blocks.empty()) return 0;
    const Cfg cfg(fn);
    const size_t n = fn.blocks.size();
    std::vector<BlockId> remap(n, kNoBlock);
    BlockId next = 0;
    for (size_t b = 0; b < n; ++b) {
        if (cfg.reachable(static_cast<BlockId>(b))) remap[b] = next++;
    }
    const size_t removed = n - next;
    if (removed == 0) return 0;

    std::vector<il::Block> kept;
    kept.reserve(next);
    for (size_t b = 0; b < n; ++b) {
        if (remap[b] == kNoBlock) continue;
        il::Block block = std::move(fn.blocks[b]);
        block.id = remap[b];
        block.handler = block.handler != kNoBlock && block.handler < n ? remap[block.handler] : kNoBlock;
        // A reachable block's normal successors are reachable, so these are
        // all live; the arguments travel unchanged.
        if (!block.instructions.empty()) {
            il::Instruction& term = block.instructions.back();
            if (term.op == il::Op::Jump || term.op == il::Op::Branch) {
                term.target.block = remap[term.target.block];
            }
            if (term.op == il::Op::Branch) term.elseTarget.block = remap[term.elseTarget.block];
        }
        kept.push_back(std::move(block));
    }
    fn.blocks = std::move(kept);
    return removed;
}

}  // namespace bronze::opt
