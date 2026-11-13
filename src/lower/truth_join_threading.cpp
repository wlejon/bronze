#include "lower/truth_join_threading.h"

#include <algorithm>
#include <vector>

#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

// How many joins one function has threaded at most: each one re-reads the
// function's definitions and uses, since it moved edges the next one counts.
constexpr int kMaxThreaded = 4096;

bool isTruthiness(const il::Instruction& inst) {
    return inst.op == il::Op::Unbox && inst.type == il::Type::Bool && !inst.rawUnbox && !inst.nullishUnbox &&
           inst.operands.size() == 1;
}

bool isBoxedBool(const il::Instruction& inst) {
    return inst.op == il::Op::Box && inst.boxType == il::Type::Bool && inst.operands.size() == 1;
}

bool mentions(const il::BlockTarget& t, il::ValueId v) {
    return std::find(t.args.begin(), t.args.end(), v) != t.args.end();
}

bool sameCopy(const il::Block& a, const il::Block& b) {
    return a.copyClass == b.copyClass && a.copyRegion == b.copyRegion;
}

class Threader {
public:
    explicit Threader(il::Function& fn) : fn_(fn) {}

    bool run() {
        bool any = false;
        for (int n = 0; n < kMaxThreaded; ++n) {
            index();
            bool changed = false;
            for (il::Block& join : fn_.blocks) {
                if (join.id == fn_.blocks.front().id) continue;
                if (truthJoin(join)) {
                    changed = threadTruth(join);
                } else if (forwarder(join)) {
                    changed = threadForward(join);
                }
                if (changed) break;
            }
            if (!changed) break;
            any = true;
        }
        return any;
    }

private:
    il::Function& fn_;
    std::vector<DefSite> defs_;
    std::vector<uint32_t> uses_;
    Cfg cfg_;

    void index() {
        defs_ = computeDefSites(fn_);
        cfg_ = buildCfg(fn_);
        uses_.assign(fn_.valueCount, 0);
        auto use = [&](il::ValueId v) {
            if (v < uses_.size()) ++uses_[v];
        };
        for (const il::Block& blk : fn_.blocks) {
            for (const il::Instruction& inst : blk.instructions) {
                for (il::ValueId v : inst.operands) use(v);
                for (il::ValueId v : inst.target.args) use(v);
                for (il::ValueId v : inst.elseTarget.args) use(v);
            }
        }
    }

    // An edge later in reverse post-order, so no back edge: an edge threaded
    // past a join only ever moves down the graph's acyclic part.
    bool forward(il::BlockId from, il::BlockId to) const {
        const uint32_t a = cfg_.rpoIndex[from];
        const uint32_t b = cfg_.rpoIndex[to];
        return a != UINT32_MAX && b != UINT32_MAX && b > a;
    }

    const il::Instruction* defOf(il::ValueId v) const {
        if (v >= defs_.size() || defs_[v].block == il::kNoBlock || defs_[v].index == UINT32_MAX) return nullptr;
        return &fn_.blocks[defs_[v].block].instructions[defs_[v].index];
    }

    // `unbox.bool p; br` and nothing else, `p` its one parameter, read
    // nowhere else, and neither named by the branch's arguments.
    bool truthJoin(const il::Block& join) const {
        if (join.params.size() != 1 || join.instructions.size() != 2) return false;
        const il::ValueId p = join.params[0].id;
        const il::Instruction& test = join.instructions[0];
        const il::Instruction& br = join.instructions[1];
        if (!isTruthiness(test) || test.operands[0] != p || br.op != il::Op::Branch || br.operands.size() != 1 ||
            br.operands[0] != test.result) {
            return false;
        }
        if (uses_[p] != 1 || uses_[test.result] != 1) return false;
        return !mentions(br.target, p) && !mentions(br.elseTarget, p) && !mentions(br.target, test.result) &&
               !mentions(br.elseTarget, test.result) && forward(join.id, br.target.block) &&
               forward(join.id, br.elseTarget.block);
    }

    // A lone `jump` whose parameters are read by its arguments alone.
    bool forwarder(const il::Block& join) const {
        if (join.instructions.size() != 1 || join.instructions[0].op != il::Op::Jump) return false;
        const il::BlockTarget& t = join.instructions[0].target;
        if (!forward(join.id, t.block)) return false;
        for (const il::BlockParam& p : join.params) {
            const auto n = static_cast<uint32_t>(std::count(t.args.begin(), t.args.end(), p.id));
            if (n != uses_[p.id]) return false;
        }
        return true;
    }

    // The edges into `join` from blocks of its own copy, as (block, which
    // target: 0 the then/jump target, 1 the else target).
    std::vector<std::pair<il::BlockId, int>> edgesInto(const il::Block& join) const {
        std::vector<std::pair<il::BlockId, int>> out;
        for (const il::Block& blk : fn_.blocks) {
            if (blk.id == join.id || blk.instructions.empty() || !sameCopy(blk, join)) continue;
            const il::Instruction& term = blk.instructions.back();
            if (term.op == il::Op::Jump && term.target.block == join.id) out.emplace_back(blk.id, 0);
            if (term.op == il::Op::Branch) {
                if (term.target.block == join.id) out.emplace_back(blk.id, 0);
                if (term.elseTarget.block == join.id) out.emplace_back(blk.id, 1);
            }
        }
        return out;
    }

    bool threadTruth(const il::Block& join) {
        const il::Instruction& br = join.instructions[1];
        const il::Block& onTrue = fn_.blocks[br.target.block];
        const il::Block& onFalse = fn_.blocks[br.elseTarget.block];
        if (!sameCopy(onTrue, join) || !sameCopy(onFalse, join)) return false;
        bool changed = false;
        for (const auto& [from, side] : edgesInto(join)) {
            il::Instruction& term = fn_.blocks[from].instructions.back();
            il::BlockTarget& edge = side == 0 ? term.target : term.elseTarget;
            if (edge.args.size() != 1) continue;
            const il::ValueId arg = edge.args[0];
            const il::Instruction* argDef = defOf(arg);
            if (term.op == il::Op::Jump) {
                if (argDef == nullptr || !isBoxedBool(*argDef)) continue;
                // The jump becomes the join's branch on the unboxed answer.
                // Build nb fresh rather than copying br: truthJoin proved
                // br.operands.size() == 1, which trips GCC 14's -Warray-bounds
                // inside std::vector's copy constructor.
                il::Instruction nb;
                nb.op = il::Op::Branch;
                nb.immI32 = br.immI32;
                nb.operands = {argDef->operands[0]};
                nb.target = br.target;
                nb.elseTarget = br.elseTarget;
                nb.span = term.span;
                nb.inlineFrame = term.inlineFrame;
                term = std::move(nb);
                changed = true;
                continue;
            }
            // A branch: the edge's side of its condition is the answer when
            // the argument is that condition boxed, or the condition is the
            // argument's truthiness.
            const il::ValueId cond = term.operands[0];
            const il::Instruction* condDef = defOf(cond);
            const bool known = (argDef != nullptr && isBoxedBool(*argDef) && argDef->operands[0] == cond) ||
                               (condDef != nullptr && isTruthiness(*condDef) && condDef->operands[0] == arg);
            const il::BlockTarget& next = side == 0 ? br.target : br.elseTarget;
            const il::BlockTarget& other = side == 0 ? term.elseTarget : term.target;
            if (!known || other.block == next.block) continue;
            edge = next;
            changed = true;
        }
        return changed;
    }

    bool threadForward(const il::Block& join) {
        const il::BlockTarget out = join.instructions[0].target;
        if (!sameCopy(fn_.blocks[out.block], join)) return false;
        bool changed = false;
        for (const auto& [from, side] : edgesInto(join)) {
            il::Instruction& term = fn_.blocks[from].instructions.back();
            il::BlockTarget& edge = side == 0 ? term.target : term.elseTarget;
            if (edge.args.size() != join.params.size()) continue;
            const il::BlockTarget& other = side == 0 ? term.elseTarget : term.target;
            if (term.op == il::Op::Branch && other.block == out.block) continue;
            il::BlockTarget next = out;
            for (il::ValueId& v : next.args) {
                for (size_t i = 0; i < join.params.size(); ++i) {
                    if (v == join.params[i].id) v = edge.args[i];
                }
            }
            edge = next;
            changed = true;
        }
        return changed;
    }
};

// Drops the blocks the threading left unreached (`reachedBefore` and no
// longer reached), keeping any that a block kept still names, and renumbers
// the rest in order (pruneUnreachable's renumbering). A block that was
// unreached before stays: it is lowering's, not this pass's.
void dropThreadedAway(il::Function& fn, const std::vector<uint8_t>& reachedBefore) {
    const Cfg cfg = buildCfg(fn);
    const size_t n = fn.blocks.size();
    std::vector<uint8_t> drop(n, 0);
    for (size_t b = 0; b < n; ++b) drop[b] = reachedBefore[b] && cfg.rpoIndex[b] == UINT32_MAX;
    for (bool again = true; again;) {
        again = false;
        for (size_t b = 0; b < n; ++b) {
            if (drop[b]) continue;
            auto keep = [&](il::BlockId t) {
                if (t != il::kNoBlock && t < n && drop[t]) {
                    drop[t] = 0;
                    again = true;
                }
            };
            keep(fn.blocks[b].handler);
            for (const il::Instruction& inst : fn.blocks[b].instructions) {
                keep(inst.target.block);
                keep(inst.elseTarget.block);
            }
        }
    }
    std::vector<il::BlockId> remap(n, il::kNoBlock);
    il::BlockId next = 0;
    for (size_t b = 0; b < n; ++b) {
        if (!drop[b]) remap[b] = next++;
    }
    if (next == n) return;
    std::vector<il::Block> kept;
    kept.reserve(next);
    for (size_t b = 0; b < n; ++b) {
        if (drop[b]) continue;
        il::Block block = std::move(fn.blocks[b]);
        block.id = remap[b];
        if (block.handler != il::kNoBlock) block.handler = remap[block.handler];
        for (il::Instruction& inst : block.instructions) {
            for (il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                if (t->block != il::kNoBlock && t->block < n) t->block = remap[t->block];
            }
        }
        kept.push_back(std::move(block));
    }
    fn.blocks = std::move(kept);
}

}  // namespace

bool definitionsDominate(const il::Function& fn) {
    const Cfg cfg = buildCfg(fn);
    const std::vector<DefSite> defs = computeDefSites(fn);
    auto ok = [&](il::ValueId v, il::BlockId useBlock, size_t useIndex) {
        if (v >= defs.size()) return false;
        const DefSite& def = defs[v];
        if (def.block == il::kNoBlock) return v < fn.params.size();
        if (def.block == useBlock) return def.index == UINT32_MAX || def.index < useIndex;
        return cfg.dominates(def.block, useBlock);
    };
    for (const il::Block& block : fn.blocks) {
        // A block nothing reaches is no use (lowering leaves them, as a
        // `finally`'s unreachable copy).
        if (cfg.rpoIndex[block.id] == UINT32_MAX) continue;
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const il::Instruction& inst = block.instructions[i];
            for (il::ValueId v : inst.operands) {
                if (!ok(v, block.id, i)) return false;
            }
            for (const il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                for (il::ValueId v : t->args) {
                    if (!ok(v, block.id, i)) return false;
                }
            }
        }
    }
    return true;
}

bool threadTruthJoins(il::Module& module) {
    bool changed = false;
    for (il::Function& fn : module.functions) {
        if (fn.isExternal || fn.blocks.size() < 3) continue;
        il::Function before = fn;
        std::vector<uint8_t> reached;
        {
            const Cfg cfg = buildCfg(fn);
            for (uint32_t r : cfg.rpoIndex) reached.push_back(r != UINT32_MAX ? 1 : 0);
        }
        if (!Threader(fn).run()) continue;
        // The joins every edge now passes by: nothing reaches them, and their
        // arguments' definitions no longer dominate them.
        dropThreadedAway(fn, reached);
        if (!definitionsDominate(fn)) {
            fn = std::move(before);
            continue;
        }
        changed = true;
    }
    return changed;
}

}  // namespace bronze::lower
