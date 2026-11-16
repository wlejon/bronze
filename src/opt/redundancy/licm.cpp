// Job 4: LOOP-INVARIANT CODE MOTION.
//
// Loops are taken innermost first, each with a preheader (cfg_edit.h
// ensurePreheader). Within a loop, its own blocks (not those of a loop nested
// in it, whose invariants were hoisted when it was done) are scanned in
// reverse postorder, header first, and each instruction is HOISTED — moved to
// the end of the preheader, before its jump to the header — when:
//   - it has a result, writes nothing, calls nothing unknown, cannot throw,
//     allocates nothing, has no observable order and is no barrier: running
//     it once before the loop, or on an iteration where it would not have
//     run, changes nothing but where its result is computed;
//   - every operand is invariant: defined outside the loop or already
//     hoisted;
//   - if it READS memory, nothing the loop body writes kills that read
//     (location.h, the one rule, over the union of the body's writes): its
//     value is the same on every iteration;
//   - if it is GUARDED, the licence it holds where it stands (licence.h) also
//     holds, identically, at the preheader's end — counting the guards
//     hoisted before it — and the loop writes nothing the licence depends on.
// Throwing instructions never move: a throw before the loop where the loop
// would have thrown later (or never) is a different program.
//
// GUARDS. A `br cond, fast, exit` whose exit block ends in `leave` is a guard.
// It is hoisted when its condition is invariant (after the hoisting above),
// its block dominates every latch (it runs on every iteration that goes
// round, so hoisting it makes no loop leave that would not have), and the
// speculation step names a resume point for the loop's entry
// (RedundancyOptions::preheaderResume). The preheader's jump then becomes
//     br cond, cont, exit'      (arms in the guard's own orientation)
// with `cont` the new end of the preheader chain (jumping to the header) and
// `exit'` a fresh cold block ending in `leave` to that resume point, after
// the cache fills of the guard's own exit (isCacheFill; a guard whose fills
// read a value of the loop stays). The
// guard in the loop stays as it was: its condition is now a value fact on
// entry to the loop, which the next value walk (job 2) folds, deleting the
// in-loop exit. Hoisting a guard executes its test before work the first
// iteration did ahead of it; that is sound because the test reads only
// invariant values, so it fails before the loop exactly when it would have
// failed on the first iteration, and G then re-runs the loop from its entry.
// WITHOUT a resume point at the loop's entry the guard stays in the loop and
// is tested on every iteration: its facts reach the header only from outside,
// never round the back edge.
//
// Nothing moves out of a loop that holds a barrier (a resume point of G): a
// value computed before the loop and used after the point would have to be
// in the point's state, and it is not. Resume points themselves never move.

#include <algorithm>
#include <cassert>
#include <optional>
#include <vector>

#include "il/effects.h"
#include "opt/core/cfg.h"
#include "opt/core/cfg_edit.h"
#include "opt/core/edge_facts.h"
#include "opt/core/fact_env.h"
#include "opt/core/loops.h"
#include "opt/core/uses.h"
#include "opt/redundancy/jobs.h"
#include "opt/redundancy/licence.h"
#include "opt/redundancy/location.h"

namespace bronze::opt::redundancy {

namespace {

using il::Op;

// The snapshots one loop's planning reads. Rebuilt for every loop: applying a
// plan moves instructions and adds blocks.
struct Analyses {
    explicit Analyses(const il::Function& fn) : cfg(fn), dom(cfg), loops(cfg, dom), defs(fn), alias(fn) {}

    Cfg cfg;
    DomTree dom;
    LoopForest loops;
    DefUse defs;
    AliasInfo alias;
};

// One step of a loop's plan, applied in order at the end of the preheader
// chain.
struct Step {
    enum class Kind : uint8_t { Move, Guard };
    Kind kind = Kind::Move;
    // The instruction to move (Move), or the guard's block (Guard), as the
    // plan's snapshot numbers them.
    BlockId block = kNoBlock;
    uint32_t index = 0;
    // Guard only.
    ValueId cond = kNoValue;
    bool exitOnTrue = false;
    LeaveTarget target;
    // The cache fills the guard's own exit made before leaving, copied to the
    // hoisted exit (cacheFills).
    std::vector<il::Instruction> fills;
};

// What the facts say inside one loop and at its preheader's end.
struct LoopFacts {
    std::vector<std::optional<Licence>> licences;  // by the guarded instruction's result
    std::vector<Fact> atPreheaderEnd;
};

bool endsInLeave(const il::Block& block) {
    return !block.instructions.empty() && block.instructions.back().op == Op::Leave;
}

// The CACHE FILLS of a guard's exit: `proto.publish` and `member.publish`,
// which ask the runtime to fill the cell the guard tested (il_op.h) and run
// no user code. Without them the cell is never filled and the guard fails
// for good, so a hoisted guard's exit makes them too.
bool isCacheFill(Op op) { return op == Op::ProtoPublish || op == Op::MemberPublish; }

// Gives every loop a preheader where one can be made. Each new block changes
// the CFG, so the loops are found afresh after each.
void ensurePreheaders(il::Function& fn) {
    const size_t bound = fn.blocks.size() + 1;
    for (size_t round = 0; round < bound; ++round) {
        const Cfg cfg(fn);
        const DomTree dom(cfg);
        const LoopForest loops(cfg, dom);
        bool made = false;
        for (uint32_t i = 0; i < loops.loops().size() && !made; ++i) {
            if (loops.preheader(cfg, i) != kNoBlock) continue;
            made = ensurePreheader(fn, cfg, loops, i) != kNoBlock;
        }
        if (!made) return;
    }
}

LoopFacts gatherFacts(const il::Function& fn, const Analyses& a, const Loop& loop, BlockId preheader) {
    LoopFacts out;
    out.licences.resize(fn.valueCount);
    FactEnv env;
    walkWithFacts(fn, a.cfg, a.dom, a.defs, env, [&](BlockId b, FactEnv& facts) {
        const bool inLoop = loop.contains(b);
        for (const il::Instruction& inst : fn.blocks[b].instructions) {
            if (inLoop && inst.result != kNoValue && inst.result < out.licences.size() &&
                il::effectsOf(inst).guarded) {
                out.licences[inst.result] = licenceAt(inst, facts);
            }
            facts.stepOver(inst);
        }
        if (b == preheader) out.atPreheaderEnd = facts.live();
    });
    return out;
}

bool loopHasBarrier(const il::Function& fn, const Loop& loop) {
    for (BlockId b : loop.blocks) {
        for (const il::Instruction& inst : fn.blocks[b].instructions) {
            if (il::effectsOf(inst).barrier) return true;
        }
    }
    return false;
}

class LoopPlanner {
public:
    LoopPlanner(JobContext& ctx, const Analyses& a, uint32_t loopIndex, BlockId preheader, LoopFacts facts)
        : ctx_(ctx),
          fn_(ctx.fn),
          a_(a),
          loopIndex_(loopIndex),
          loop_(a.loops.loop(loopIndex)),
          preheader_(preheader),
          facts_(std::move(facts)),
          hoisted_(ctx.fn.valueCount, 0) {
        for (BlockId b : loop_.blocks) loopWrites_.addAll(blockWrites(b));
        preheaderFacts_.push();
        preheaderFacts_.addAll(facts_.atPreheaderEnd);
    }

    std::vector<Step> plan();

private:
    WriteSet blockWrites(BlockId b) const;
    bool invariant(ValueId v) const;
    bool canHoist(const il::Instruction& inst) const;
    bool dominatesLatches(BlockId b) const;
    std::optional<Step> guardStep(BlockId b);
    const std::optional<LeaveTarget>& resumeTarget();

    JobContext& ctx_;
    il::Function& fn_;
    const Analyses& a_;
    uint32_t loopIndex_;
    const Loop& loop_;
    BlockId preheader_;
    LoopFacts facts_;
    // The facts at the end of the preheader chain as the plan grows it: the
    // preheader's own, then each hoisted guard's fast edge.
    FactEnv preheaderFacts_;
    WriteSet loopWrites_;
    std::vector<uint8_t> hoisted_;
    // The speculation step's answer for this loop, asked once.
    bool resumeAsked_ = false;
    std::optional<LeaveTarget> resume_;
};

WriteSet LoopPlanner::blockWrites(BlockId b) const {
    WriteSet out;
    for (const il::Instruction& inst : fn_.blocks[b].instructions) out.add(writeOf(inst, il::effectsOf(inst)));
    return out;
}

bool LoopPlanner::invariant(ValueId v) const {
    const DefSite& d = a_.defs.def(v);
    switch (d.kind) {
        case DefSite::Kind::Param:
            return true;
        case DefSite::Kind::BlockParam:
            return !loop_.contains(d.block);
        case DefSite::Kind::Instruction:
            return !loop_.contains(d.block) || (v < hoisted_.size() && hoisted_[v]);
        case DefSite::Kind::None:
            return false;
    }
    return false;
}

bool LoopPlanner::canHoist(const il::Instruction& inst) const {
    if (inst.result == kNoValue || inst.result >= hoisted_.size() || il::isTerminator(inst.op)) return false;
    const il::Effects e = il::effectsOf(inst);
    if (!e.writes.empty() || e.callsUnknown || e.throws || e.allocates || e.observableOrder || e.barrier) {
        return false;
    }
    for (ValueId v : inst.operands) {
        if (!invariant(v)) return false;
    }
    if (e.guarded) {
        const std::optional<Licence>& licence = facts_.licences[inst.result];
        if (!licence || !licenceHolds(*licence, preheaderFacts_)) return false;
        if (licence->dependsOn.intersects(loopWrites_.allLocs())) return false;
    }
    if (!e.reads.empty() && loopWrites_.kills(readOf(inst, e), a_.alias)) return false;
    return true;
}

bool LoopPlanner::dominatesLatches(BlockId b) const {
    for (BlockId latch : loop_.latches) {
        if (!a_.dom.dominates(b, latch)) return false;
    }
    return true;
}

const std::optional<LeaveTarget>& LoopPlanner::resumeTarget() {
    if (!resumeAsked_) {
        resumeAsked_ = true;
        if (fn_.isSpeculated() && ctx_.options.preheaderResume) {
            resume_ = ctx_.options.preheaderResume(fn_, loop_.header, preheader_,
                                                   [this](ValueId v) { return ctx_.current(v); });
            if (resume_ && !resume_->buildState) resume_.reset();
        }
    }
    return resume_;
}

std::optional<Step> LoopPlanner::guardStep(BlockId b) {
    const il::Instruction& term = fn_.blocks[b].instructions.back();
    if (term.op != Op::Branch || term.operands.empty()) return std::nullopt;
    const BlockId thenBlock = term.target.block;
    const BlockId elseBlock = term.elseTarget.block;
    if (thenBlock == elseBlock) return std::nullopt;
    const bool thenExits = endsInLeave(fn_.blocks[thenBlock]);
    const bool elseExits = endsInLeave(fn_.blocks[elseBlock]);
    if (thenExits == elseExits) return std::nullopt;
    const ValueId cond = term.operands[0];
    if (!invariant(cond) || !dominatesLatches(b)) return std::nullopt;
    // The exit's cache fills go with the guard, so their operands must be
    // there at the preheader's end too.
    std::vector<il::Instruction> fills;
    for (const il::Instruction& inst : fn_.blocks[thenExits ? thenBlock : elseBlock].instructions) {
        if (!isCacheFill(inst.op)) continue;
        for (ValueId v : inst.operands) {
            if (!invariant(v)) return std::nullopt;
        }
        fills.push_back(inst);
    }
    const std::optional<LeaveTarget>& target = resumeTarget();
    if (!target) return std::nullopt;

    // From here on the preheader chain is past this guard: its fast edge's
    // facts hold there. They were derived at the guard's own block, about
    // invariant values whose tests move with them.
    const EdgeFacts edge = branchFacts(fn_, a_.defs, b);
    preheaderFacts_.addAll(thenExits ? edge.onFalse : edge.onTrue);

    Step step;
    step.kind = Step::Kind::Guard;
    step.block = b;
    step.cond = cond;
    step.exitOnTrue = thenExits;
    step.target = *target;
    step.fills = std::move(fills);
    return step;
}

std::vector<Step> LoopPlanner::plan() {
    std::vector<Step> steps;
    for (BlockId b : a_.cfg.rpo()) {
        if (!loop_.contains(b) || a_.loops.innermost(b) != loopIndex_) continue;
        const auto& insts = fn_.blocks[b].instructions;
        for (size_t i = 0; i + 1 < insts.size(); ++i) {
            if (!canHoist(insts[i])) continue;
            hoisted_[insts[i].result] = 1;
            Step step;
            step.kind = Step::Kind::Move;
            step.block = b;
            step.index = static_cast<uint32_t>(i);
            steps.push_back(std::move(step));
        }
        if (!insts.empty()) {
            if (std::optional<Step> guard = guardStep(b)) steps.push_back(std::move(*guard));
        }
    }
    return steps;
}

void insertBeforeTerminator(il::Function& fn, BlockId b, il::Instruction inst) {
    auto& insts = fn.blocks[b].instructions;
    assert(!insts.empty() && il::isTerminator(insts.back().op));
    insts.insert(insts.end() - 1, std::move(inst));
}

// Ends block `tail` (which ends in a jump to the loop header) with the
// hoisted guard instead, and answers the new end of the preheader chain.
BlockId hoistGuard(il::Function& fn, BlockId tail, const Step& step) {
    assert(fn.isSpeculated());
    const BlockId handler = fn.blocks[tail].handler;
    const bool cold = fn.blocks[tail].cold;
    il::Instruction jump = std::move(fn.blocks[tail].instructions.back());
    fn.blocks[tail].instructions.pop_back();
    assert(jump.op == Op::Jump);

    // newBlock invalidates references into fn.blocks: ids only from here.
    const BlockId cont = newBlock(fn).id;
    fn.blocks[cont].handler = handler;
    fn.blocks[cont].cold = cold;
    fn.blocks[cont].instructions.push_back(std::move(jump));

    const BlockId exit = newBlock(fn).id;
    fn.blocks[exit].handler = handler;
    fn.blocks[exit].cold = true;
    for (const il::Instruction& fill : step.fills) fn.blocks[exit].instructions.push_back(fill);
    std::vector<ValueId> state = step.target.buildState(fn, exit);
    il::Instruction leave;
    leave.op = Op::Leave;
    leave.calleeIndex = fn.speculatedFrom;
    leave.immI32 = static_cast<int32_t>(step.target.resumeId);
    leave.operands = std::move(state);
    fn.blocks[exit].instructions.push_back(std::move(leave));

    il::Instruction br;
    br.op = Op::Branch;
    br.operands = {step.cond};
    br.target.block = step.exitOnTrue ? exit : cont;
    br.elseTarget.block = step.exitOnTrue ? cont : exit;
    fn.blocks[tail].instructions.push_back(std::move(br));
    return cont;
}

// Applies a plan made against the function as it stood. Moves copy out of the
// loop's blocks, which the chain never is, so the plan's indices stay valid
// until the originals are erased at the end.
bool applyPlan(JobContext& ctx, BlockId preheader, const std::vector<Step>& steps) {
    il::Function& fn = ctx.fn;
    if (steps.empty()) return false;
    std::vector<std::vector<uint8_t>> moved(fn.blocks.size());
    BlockId tail = preheader;
    for (const Step& step : steps) {
        if (step.kind == Step::Kind::Guard) {
            tail = hoistGuard(fn, tail, step);
            ++ctx.stats.guardsHoisted;
            continue;
        }
        assert(step.block != tail);
        auto& marks = moved[step.block];
        if (marks.empty()) marks.assign(fn.blocks[step.block].instructions.size(), 0);
        marks[step.index] = 1;
        insertBeforeTerminator(fn, tail, fn.blocks[step.block].instructions[step.index]);
        ++ctx.stats.hoisted;
    }
    for (size_t b = 0; b < moved.size(); ++b) {
        if (moved[b].empty()) continue;
        auto& insts = fn.blocks[b].instructions;
        std::vector<il::Instruction> kept;
        kept.reserve(insts.size());
        for (size_t i = 0; i < insts.size(); ++i) {
            if (!moved[b][i]) kept.push_back(std::move(insts[i]));
        }
        insts = std::move(kept);
    }
    return true;
}

bool hoistLoop(JobContext& ctx, const Analyses& a, uint32_t loopIndex) {
    const Loop& loop = a.loops.loop(loopIndex);
    const BlockId preheader = a.loops.preheader(a.cfg, loopIndex);
    if (preheader == kNoBlock || loopHasBarrier(ctx.fn, loop)) return false;
    LoopFacts facts = gatherFacts(ctx.fn, a, loop, preheader);
    LoopPlanner planner(ctx, a, loopIndex, preheader, std::move(facts));
    return applyPlan(ctx, preheader, planner.plan());
}

// The next loop to do: the deepest not yet done, lowest header first.
uint32_t nextLoop(const LoopForest& loops, const std::vector<BlockId>& done) {
    uint32_t best = kNoLoop;
    for (uint32_t i = 0; i < loops.loops().size(); ++i) {
        const Loop& l = loops.loop(i);
        if (std::find(done.begin(), done.end(), l.header) != done.end()) continue;
        if (best == kNoLoop || l.depth > loops.loop(best).depth ||
            (l.depth == loops.loop(best).depth && l.header < loops.loop(best).header)) {
            best = i;
        }
    }
    return best;
}

}  // namespace

bool runLoopHoisting(JobContext& ctx) {
    il::Function& fn = ctx.fn;
    if (!ctx.options.hoistLoops || fn.blocks.empty()) return false;
    ensurePreheaders(fn);
    // Headers keep their ids here: this job adds blocks and never renumbers.
    std::vector<BlockId> done;
    bool changed = false;
    const size_t bound = fn.blocks.size() + 1;
    for (size_t round = 0; round < bound; ++round) {
        const Analyses a(fn);
        const uint32_t next = nextLoop(a.loops, done);
        if (next == kNoLoop) break;
        done.push_back(a.loops.loop(next).header);
        changed |= hoistLoop(ctx, a, next);
    }
    return changed;
}

}  // namespace bronze::opt::redundancy
