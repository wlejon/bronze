// Job 6: DEAD CODE ELIMINATION.
//
//   1. TRIVIAL block parameters: a parameter every incoming edge passes the
//      same value v (or itself, around a loop) is v. v dominates the block:
//      the first arrival on any path comes from an edge passing v, and an edge
//      passing the parameter itself leaves a block the parameter's block
//      dominates. So every use of the parameter may read v instead.
//   2. LIVE VALUES, marked from the roots outward: every operand of an
//      instruction that is not removableIfUnused (il/effects.h) or is a
//      terminator is live; a live instruction's operands are live; a live
//      block parameter makes the matching argument of every incoming edge
//      live. Edge arguments are NOT roots — a parameter used only to compute
//      its own next value around a loop dies with its cycle. Everything
//      removable that is not live goes, and every parameter that is not live
//      goes with its argument on every edge.
//   3. FORWARDING blocks: a block holding only `jump T(args)` is bypassed,
//      each predecessor jumping (or branching) straight to T with the
//      arguments substituted. Not when T is a loop header (that block is the
//      loop's preheader, which job 4 needs and would only make again), not
//      for a handler (entered by throw edges, which cannot move), and not
//      when a predecessor's branch would end with both arms on T.
//   4. Unreachable blocks go.
// `removeUnnamedResumePoints` is the resume-point half, run by the driver
// before the first round on a generic function.

#include <algorithm>
#include <cassert>
#include <vector>

#include "il/effects.h"
#include "opt/core/cfg.h"
#include "opt/core/cfg_edit.h"
#include "opt/core/uses.h"
#include "opt/redundancy/jobs.h"

namespace bronze::opt::redundancy {

namespace {

using il::Op;

// An edge into a block, named by the block whose terminator carries it.
struct InEdge {
    BlockId from;
    bool isElse;  // the branch's else-target; otherwise the jump or then-target
};

// Every normal edge into each block, from the terminators as they stand.
std::vector<std::vector<InEdge>> incomingEdges(const il::Function& fn) {
    std::vector<std::vector<InEdge>> in(fn.blocks.size());
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const auto& insts = fn.blocks[b].instructions;
        if (insts.empty()) continue;
        const il::Instruction& term = insts.back();
        const BlockId from = static_cast<BlockId>(b);
        if ((term.op == Op::Jump || term.op == Op::Branch) && term.target.block < in.size()) {
            in[term.target.block].push_back({from, false});
        }
        if (term.op == Op::Branch && term.elseTarget.block < in.size()) {
            in[term.elseTarget.block].push_back({from, true});
        }
    }
    return in;
}

std::vector<ValueId>& argsOf(il::Function& fn, const InEdge& e) {
    il::Instruction& term = fn.blocks[e.from].instructions.back();
    return e.isElse ? term.elseTarget.args : term.target.args;
}

// Drops parameter `k` of block `b` and the matching argument of every edge
// into it.
void eraseParam(il::Function& fn, const std::vector<InEdge>& in, BlockId b, size_t k) {
    auto& params = fn.blocks[b].params;
    params.erase(params.begin() + static_cast<std::ptrdiff_t>(k));
    for (const InEdge& e : in) {
        auto& args = argsOf(fn, e);
        assert(k < args.size());
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(k));
    }
}

// The one value every edge passes for parameter `k` of `b`, other than the
// parameter itself; kNoValue if they disagree or there are no edges.
ValueId trivialValue(il::Function& fn, const std::vector<InEdge>& in, BlockId b, size_t k) {
    const ValueId self = fn.blocks[b].params[k].id;
    ValueId only = kNoValue;
    for (const InEdge& e : in) {
        const ValueId a = argsOf(fn, e)[k];
        if (a == self) continue;
        if (only != kNoValue && a != only) return kNoValue;
        only = a;
    }
    return only;
}

bool removeTrivialParams(JobContext& ctx) {
    il::Function& fn = ctx.fn;
    bool changed = false;
    // A replacement can make another parameter trivial; the bound is the
    // number of parameters there could be to remove.
    for (bool again = true; again;) {
        again = false;
        const auto in = incomingEdges(fn);
        for (size_t b = 1; b < fn.blocks.size(); ++b) {
            const BlockId id = static_cast<BlockId>(b);
            for (size_t k = fn.blocks[b].params.size(); k-- > 0;) {
                const ValueId v = trivialValue(fn, in[b], id, k);
                if (v == kNoValue) continue;
                const ValueId p = fn.blocks[b].params[k].id;
                eraseParam(fn, in[b], id, k);
                replaceAllUses(fn, p, v);
                ++ctx.stats.trivialBlockParams;
                changed = again = true;
            }
        }
    }
    return changed;
}

// Instructions nothing may delete, whatever uses their result.
bool isRoot(const il::Instruction& inst) {
    return il::isTerminator(inst.op) || !il::effectsOf(inst).removableIfUnused;
}

bool sweepDeadValues(JobContext& ctx) {
    il::Function& fn = ctx.fn;
    const size_t n = fn.valueCount;
    // Where each value is defined: an instruction (block, index) or a block
    // parameter (block, k). Function parameters need nothing.
    struct Def {
        BlockId block = kNoBlock;
        uint32_t index = 0;
        bool param = false;
    };
    std::vector<Def> defs(n);
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const il::Block& block = fn.blocks[b];
        for (size_t k = 0; k < block.params.size(); ++k) {
            if (block.params[k].id < n) defs[block.params[k].id] = {static_cast<BlockId>(b), uint32_t(k), true};
        }
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const ValueId r = block.instructions[i].result;
            if (r < n) defs[r] = {static_cast<BlockId>(b), uint32_t(i), false};
        }
    }
    const auto in = incomingEdges(fn);
    std::vector<uint8_t> live(n, 0);
    std::vector<ValueId> work;
    auto mark = [&](ValueId v) {
        if (v < n && !live[v]) {
            live[v] = 1;
            work.push_back(v);
        }
    };
    for (const il::Block& block : fn.blocks) {
        for (const il::Instruction& inst : block.instructions) {
            if (isRoot(inst)) {
                for (ValueId v : inst.operands) mark(v);
            }
        }
    }
    while (!work.empty()) {
        const ValueId v = work.back();
        work.pop_back();
        const Def& d = defs[v];
        if (d.block == kNoBlock) continue;
        if (d.param) {
            for (const InEdge& e : in[d.block]) mark(argsOf(fn, e)[d.index]);
        } else {
            for (ValueId u : fn.blocks[d.block].instructions[d.index].operands) mark(u);
        }
    }

    bool changed = false;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        auto& insts = fn.blocks[b].instructions;
        const size_t before = insts.size();
        insts.erase(std::remove_if(insts.begin(), insts.end(),
                                   [&](const il::Instruction& inst) {
                                       return !isRoot(inst) && (inst.result >= n || !live[inst.result]);
                                   }),
                    insts.end());
        ctx.stats.deadInstructions += before - insts.size();
        changed |= before != insts.size();
    }
    for (size_t b = 1; b < fn.blocks.size(); ++b) {
        for (size_t k = fn.blocks[b].params.size(); k-- > 0;) {
            const ValueId p = fn.blocks[b].params[k].id;
            if (p < n && live[p]) continue;
            eraseParam(fn, in[b], static_cast<BlockId>(b), k);
            ++ctx.stats.deadBlockParams;
            changed = true;
        }
    }
    return changed;
}

// A block that only jumps on, and may be bypassed (see the header); kNoBlock
// when there is none.
BlockId findForwarder(const il::Function& fn, const Cfg& cfg, const DomTree& dom,
                      const std::vector<uint8_t>& isHandler) {
    for (size_t b = 1; b < fn.blocks.size(); ++b) {
        const BlockId f = static_cast<BlockId>(b);
        const il::Block& block = fn.blocks[b];
        if (!cfg.reachable(f) || isHandler[b] || block.instructions.size() != 1) continue;
        const il::Instruction& jump = block.instructions.back();
        if (jump.op != Op::Jump) continue;
        const BlockId t = jump.target.block;
        if (t == f || cfg.inEdges(f).empty()) continue;
        bool header = false;
        for (BlockId p : cfg.preds(t)) header |= dom.dominates(t, p);
        if (header) continue;
        bool ok = true;
        for (const Edge& e : cfg.inEdges(f)) {
            if (e.kind == EdgeKind::Throw) ok = false;
            const il::Instruction& term = fn.blocks[e.from].instructions.back();
            if (term.op == Op::Branch) {
                const BlockId other = e.kind == EdgeKind::BranchThen ? term.elseTarget.block : term.target.block;
                if (other == t || other == f) ok = false;
            }
        }
        if (ok) return f;
    }
    return kNoBlock;
}

bool removeForwarders(JobContext& ctx) {
    il::Function& fn = ctx.fn;
    bool changed = false;
    // One forwarder per CFG snapshot: retargeting edits the edges the next
    // one would be found by. Bounded by the block count.
    for (size_t round = 0; round < fn.blocks.size(); ++round) {
        const Cfg cfg(fn);
        const DomTree dom(cfg);
        std::vector<uint8_t> isHandler(fn.blocks.size(), 0);
        for (const il::Block& block : fn.blocks) {
            if (block.handler != kNoBlock && block.handler < isHandler.size()) isHandler[block.handler] = 1;
        }
        const BlockId f = findForwarder(fn, cfg, dom, isHandler);
        if (f == kNoBlock) break;
        const il::Instruction jump = fn.blocks[f].instructions.back();
        const std::vector<il::BlockParam> params = fn.blocks[f].params;
        for (const Edge& e : cfg.inEdges(f)) {
            const std::vector<ValueId> incoming = edgeArgs(fn, e);
            std::vector<ValueId> args;
            args.reserve(jump.target.args.size());
            for (ValueId a : jump.target.args) {
                ValueId v = a;
                for (size_t k = 0; k < params.size(); ++k) {
                    if (params[k].id == a) v = incoming[k];
                }
                args.push_back(v);
            }
            retargetEdge(fn, e, jump.target.block, std::move(args));
        }
        ++ctx.stats.forwardersRemoved;
        ctx.stats.blocksRemoved += removeUnreachableBlocks(fn);
        changed = true;
    }
    return changed;
}

}  // namespace

bool removeUnnamedResumePoints(JobContext& ctx) {
    il::Function& fn = ctx.fn;
    if (fn.isSpeculated() || !ctx.options.removeUnnamedResumePoints) return false;
    const std::vector<uint32_t>& named = ctx.options.namedResumeIds;
    assert(std::is_sorted(named.begin(), named.end()));
    bool changed = false;
    for (il::Block& block : fn.blocks) {
        auto& insts = block.instructions;
        const size_t before = insts.size();
        insts.erase(std::remove_if(insts.begin(), insts.end(),
                                   [&](const il::Instruction& inst) {
                                       return inst.op == Op::ResumePoint &&
                                              !std::binary_search(named.begin(), named.end(),
                                                                  static_cast<uint32_t>(inst.immI32));
                                   }),
                    insts.end());
        ctx.stats.resumePointsRemoved += before - insts.size();
        changed |= before != insts.size();
    }
    return changed;
}

bool runDeadCode(JobContext& ctx) {
    il::Function& fn = ctx.fn;
    if (fn.blocks.empty()) return false;
    // Trivial parameters and liveness both assume every edge they read comes
    // from a reachable block.
    size_t removed = removeUnreachableBlocks(fn);
    bool changed = removeTrivialParams(ctx);
    changed |= sweepDeadValues(ctx);
    changed |= removeForwarders(ctx);
    removed += removeUnreachableBlocks(fn);
    ctx.stats.blocksRemoved += removed;
    return changed || removed != 0;
}

}  // namespace bronze::opt::redundancy
