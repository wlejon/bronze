// Rule 3: a box is made where it is needed, not where its value is.
//
//   EXIT BLOCKS. A block is exit-only when it ends in `leave`, or is cold and
//   every successor is exit-only: from it, S only leaves. A `box` whose every
//   use is in exit-only blocks is copied into each of them, in front of its
//   first use there, and the original goes; the path that stays in S carries
//   only the typed value. A `leave` hands G the state its resume point
//   declares, dynamic where G's values are, so the exit blocks are where the
//   box belongs.
//
//   ONE USE. A `box` with a single use in a block U its own block strictly
//   dominates, where every loop containing U also contains the box's block,
//   moves to U, in front of the use. U runs at most as often as the box's
//   block did (it is dominated, and no loop the box was outside of repeats
//   it), and on the paths that skip U the box is no longer made.
//
// Both are exact: a box is pure, and its operand dominates every new
// position (it dominates the box's block, which dominates each use).
// Only boxes of i32, f64 and bool move; a string box allocates.
//
// A copy must not be found again by value numbering and replaced by the
// box it was copied from, so the original is deleted here, not left to dead
// code.

#include <algorithm>
#include <cassert>
#include <vector>

#include "opt/core/cfg.h"
#include "opt/core/loops.h"
#include "opt/core/uses.h"
#include "opt/repr/phases.h"
#include "opt/repr/rewrite.h"

namespace bronze::opt::repr {

namespace {

using il::Op;
using il::Type;

// A least fixpoint, so a cycle of cold blocks with no way out but a `leave`
// is not exit-only: that is a loop S may stay in.
std::vector<uint8_t> exitOnlyBlocks(const il::Function& fn, const Cfg& cfg) {
    std::vector<uint8_t> exit(fn.blocks.size(), 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (BlockId b : cfg.postorder()) {
            if (exit[b]) continue;
            const il::Block& block = fn.blocks[b];
            const il::Instruction& term = block.instructions.back();
            bool now = term.op == Op::Leave;
            if (!now && block.cold && (term.op == Op::Jump || term.op == Op::Branch)) {
                now = std::all_of(cfg.succs(b).begin(), cfg.succs(b).end(),
                                  [&](BlockId s) { return exit[s] != 0; });
            }
            if (now) {
                exit[b] = 1;
                changed = true;
            }
        }
    }
    return exit;
}

bool movableBox(const il::Instruction& inst) {
    return inst.op == Op::Box && inst.result != kNoValue && inst.operands.size() == 1 &&
           (inst.boxType == Type::I32 || inst.boxType == Type::F64 || inst.boxType == Type::Bool);
}

class Sinker {
public:
    explicit Sinker(PhaseContext& ctx)
        : ctx_(ctx), fn_(ctx.fn), cfg_(ctx.fn), dom_(cfg_), loops_(cfg_, dom_), defs_(ctx.fn),
          exit_(exitOnlyBlocks(ctx.fn, cfg_)) {}

    bool run();

private:
    bool toExits(BlockId b, size_t i, const std::vector<UseSite>& uses);
    bool toSingleUse(BlockId b, size_t i, const UseSite& use);
    // Every loop containing `to` also contains `from`.
    bool noDeeperLoop(BlockId from, BlockId to) const;

    PhaseContext& ctx_;
    il::Function& fn_;
    const Cfg cfg_;
    const DomTree dom_;
    const LoopForest loops_;
    const DefUse defs_;
    const std::vector<uint8_t> exit_;
    EditPlan edits_;
    // Use rewrites, applied after the walk: (use, new value).
    std::vector<std::pair<UseSite, ValueId>> rewrites_;
};

bool Sinker::noDeeperLoop(BlockId from, BlockId to) const {
    for (uint32_t l = loops_.innermost(to); l != kNoLoop; l = loops_.loop(l).parent) {
        if (!loops_.loop(l).contains(from)) return false;
    }
    return true;
}

bool Sinker::toExits(BlockId b, size_t i, const std::vector<UseSite>& uses) {
    for (const UseSite& u : uses) {
        if (!exit_[u.block]) return false;
    }
    const il::Instruction& box = fn_.blocks[b].instructions[i];
    // Uses come in block then instruction order: each block's first use is
    // where its copy goes.
    BlockId current = kNoBlock;
    ValueId copy = kNoValue;
    for (const UseSite& u : uses) {
        if (u.block != current) {
            current = u.block;
            assert(dom_.strictlyDominates(b, current));
            il::Instruction clone = makeBox(fn_, box.operands[0], box.boxType, box.span);
            copy = clone.result;
            edits_.insertBefore(current, u.inst, std::move(clone));
            ++ctx_.stats.exitBoxClones;
        }
        rewrites_.emplace_back(u, copy);
    }
    edits_.remove(b, i);
    ++ctx_.stats.boxesSunkToExits;
    return true;
}

bool Sinker::toSingleUse(BlockId b, size_t i, const UseSite& use) {
    if (!dom_.strictlyDominates(b, use.block) || !noDeeperLoop(b, use.block)) return false;
    const il::Instruction& box = fn_.blocks[b].instructions[i];
    il::Instruction moved = makeBox(fn_, box.operands[0], box.boxType, box.span);
    rewrites_.emplace_back(use, moved.result);
    edits_.insertBefore(use.block, use.inst, std::move(moved));
    edits_.remove(b, i);
    ++ctx_.stats.boxesSunk;
    return true;
}

bool Sinker::run() {
    bool changed = false;
    for (BlockId b : cfg_.rpo()) {
        if (exit_[b]) continue;
        const auto& insts = fn_.blocks[b].instructions;
        for (size_t i = 0; i < insts.size(); ++i) {
            if (!movableBox(insts[i])) continue;
            const std::vector<UseSite>& uses = defs_.uses(insts[i].result);
            if (uses.empty()) continue;
            if (toExits(b, i, uses)) {
                changed = true;
            } else if (uses.size() == 1 && toSingleUse(b, i, uses.front())) {
                changed = true;
            }
        }
    }
    if (!changed) return false;
    // The use sites name instructions as they stood, so they are rewritten
    // before the plan moves anything.
    for (const auto& [use, value] : rewrites_) useRef(fn_, use) = value;
    edits_.apply(fn_);
    return true;
}

}  // namespace

bool sinkBoxes(PhaseContext& ctx) {
    if (ctx.fn.blocks.empty()) return false;
    Sinker sinker(ctx);
    return sinker.run();
}

}  // namespace bronze::opt::repr
