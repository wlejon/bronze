// Jobs 1-3: one dominator-tree walk (dom_walk.h) that, at each instruction,
//   - resolves its operands through the replacements made so far;
//   - folds it (fold.h) or answers it as a test from the facts (tests.h);
//   - looks it up in the available-expression table (expr_table.h) when it
//     is REUSABLE — it writes nothing, calls nothing unknown, allocates
//     nothing, has no observable order and is no barrier — and replaces it by
//     the value already holding its answer, or records itself there;
//   - otherwise kills what its write invalidates (location.h, the one rule)
//     and, for a keyed store, records the stored value as the answer of the
//     matching load.
// A branch whose condition is known becomes a jump after the walk, and the
// blocks it no longer reaches are deleted.
//
// Why replacing a later instruction by an earlier one is right:
//   - pure: same op, immediates and operands compute the same value;
//   - a read: the table entry is alive only while no write that may change
//     the location lies on any path from the earlier read (dom_walk.h kills
//     at joins by every path's writes);
//   - a read that can THROW (`env.get.tdz`, a private access): the earlier
//     one did not throw — the walk got past it — and nothing since changed
//     what it depends on, so the later one would not throw either and
//     returns the same value. Its deletion is explicit (it is not
//     removableIfUnused);
//   - a GUARDED read: the earlier one ran under its own licence, so its
//     value is the location's real contents, and the later one's licence
//     says nothing the earlier value needs.
//
// Edits during the walk are in place only (dom_walk.h says why); deletions
// and the branch rewrites wait for `apply`.

#include <cassert>
#include <utility>

#include "il/effects.h"
#include "opt/core/cfg_edit.h"
#include "opt/core/fact_env.h"
#include "opt/core/uses.h"
#include "opt/redundancy/dom_walk.h"
#include "opt/redundancy/expr_table.h"
#include "opt/redundancy/fold.h"
#include "opt/redundancy/jobs.h"
#include "opt/redundancy/location.h"
#include "opt/redundancy/tests.h"

namespace bronze::opt::redundancy {

namespace {

using il::Op;

// The instructions the table may hold: their result is a function of their
// operands, immediates and the memory their read names, and running them a
// second time changes nothing.
bool reusable(const il::Instruction& inst, const il::Effects& e) {
    return inst.result != kNoValue && !il::isTerminator(inst.op) && e.writes.empty() && !e.callsUnknown &&
           !e.allocates && !e.observableOrder && !e.barrier;
}

class ValueWalk {
public:
    ValueWalk(JobContext& ctx, const DefUse& defs, const AliasInfo& alias, FactEnv& facts, ExprTable& exprs)
        : ctx_(ctx),
          fn_(ctx.fn),
          defs_(defs),
          values_(ctx.fn, defs),
          alias_(alias),
          facts_(facts),
          exprs_(exprs),
          repl_(ctx.fn.valueCount, kNoValue),
          dead_(ctx.fn.blocks.size()) {}

    void visitBlock(BlockId b);
    bool apply();

private:
    ValueId resolve(ValueId v) const;
    void resolveUses(il::Instruction& inst);
    void visitTerminator(BlockId b, const il::Instruction& inst);
    void visitInstruction(BlockId b, size_t i);
    // Records that instruction `i` of `b` is redundant: its result is `by`.
    void replace(BlockId b, size_t i, ValueId by);
    bool lookupAndRecord(BlockId b, size_t i, const il::Effects& e);
    void recordWrite(const il::Instruction& inst, const il::Effects& e);
    void forwardStore(const il::Instruction& inst, const Access& write);

    JobContext& ctx_;
    il::Function& fn_;
    const DefUse& defs_;
    ValueView values_;
    const AliasInfo& alias_;
    FactEnv& facts_;
    ExprTable& exprs_;
    std::vector<ValueId> repl_;
    std::vector<std::vector<uint8_t>> dead_;
    std::vector<std::pair<BlockId, bool>> branchFolds_;
    bool changed_ = false;
};

ValueId ValueWalk::resolve(ValueId v) const {
    // Replacements always point at an EARLIER value (one whose definition
    // dominates), so a chain ends.
    while (v < repl_.size() && repl_[v] != kNoValue) v = repl_[v];
    return v;
}

void ValueWalk::resolveUses(il::Instruction& inst) {
    auto fix = [&](ValueId& v) {
        const ValueId r = resolve(v);
        if (r != v) {
            v = r;
            changed_ = true;
        }
    };
    for (ValueId& v : inst.operands) fix(v);
    for (ValueId& v : inst.target.args) fix(v);
    for (ValueId& v : inst.elseTarget.args) fix(v);
}

void ValueWalk::replace(BlockId b, size_t i, ValueId by) {
    const il::Instruction& inst = fn_.blocks[b].instructions[i];
    assert(inst.result != kNoValue && by != inst.result);
    assert(defs_.type(by) == inst.type);
    repl_[inst.result] = by;
    if (dead_[b].empty()) dead_[b].assign(fn_.blocks[b].instructions.size(), 0);
    dead_[b][i] = 1;
    changed_ = true;
}

void ValueWalk::visitBlock(BlockId b) {
    const size_t n = fn_.blocks[b].instructions.size();
    for (size_t i = 0; i < n; ++i) visitInstruction(b, i);
}

void ValueWalk::visitTerminator(BlockId b, const il::Instruction& inst) {
    if (inst.op != Op::Branch || inst.operands.empty()) return;
    const ValueId cond = inst.operands[0];
    std::optional<bool> known = constBool(values_, cond);
    if (!known) known = facts_.boolValue(cond);
    if (known) branchFolds_.emplace_back(b, *known);
}

void ValueWalk::visitInstruction(BlockId b, size_t i) {
    il::Instruction& inst = fn_.blocks[b].instructions[i];
    resolveUses(inst);
    if (il::isTerminator(inst.op)) {
        visitTerminator(b, inst);
        facts_.stepOver(inst);
        return;
    }
    if (il::effectsOf(inst).barrier) {
        // A resume point: G's code after it is entered with only its state.
        exprs_.hideAll();
        facts_.stepOver(inst);
        return;
    }

    if (const ValueId answer = knownConstructResult(inst, facts_, values_); answer != kNoValue) {
        replace(b, i, answer);
        ++ctx_.stats.identitiesFolded;
        return;
    }
    const FoldOutcome folded = foldInstruction(inst, values_);
    if (folded.kind == FoldOutcome::Kind::Identity) {
        replace(b, i, folded.value);
        ++ctx_.stats.identitiesFolded;
        return;
    }
    if (folded.kind == FoldOutcome::Kind::Constant) {
        ++ctx_.stats.constantsFolded;
        changed_ = true;
    } else if (const std::optional<bool> answer = knownTestResult(inst, facts_, exprs_, values_)) {
        makeBoolConstant(inst, *answer);
        ++ctx_.stats.testsFolded;
        changed_ = true;
    }

    // Folding may have changed the op, so the effects are read afresh.
    const il::Effects e = il::effectsOf(inst);
    if (reusable(inst, e)) {
        if (lookupAndRecord(b, i, e)) return;
    } else {
        recordWrite(inst, e);
    }
    facts_.stepOver(inst);
}

bool ValueWalk::lookupAndRecord(BlockId b, size_t i, const il::Effects& e) {
    const il::Instruction& inst = fn_.blocks[b].instructions[i];
    const ExprKey key = exprKeyOf(inst);
    const Available found = exprs_.lookup(key);
    if (found.value != kNoValue && found.value != inst.result && defs_.type(found.value) == inst.type) {
        replace(b, i, found.value);
        if (e.reads.empty()) {
            ++ctx_.stats.valuesNumbered;
        } else if (found.forwarded) {
            ++ctx_.stats.loadsForwarded;
        } else {
            ++ctx_.stats.loadsEliminated;
        }
        return true;
    }
    const Access read = readOf(inst, e);
    exprs_.add(key, inst.result, read);
    // A checked env read that returned also answers the plain read of the
    // slot. (The converse does not hold: a plain read may have seen the
    // uninitialized marker, which the checked one would have refused.)
    if (inst.op == Op::EnvGetTdz && read.where.place == Place::Env) {
        exprs_.add(loadKey(Op::EnvGet, inst.type, read.where), inst.result, read);
    }
    return false;
}

void ValueWalk::recordWrite(const il::Instruction& inst, const il::Effects& e) {
    const Access write = writeOf(inst, e);
    if (write.empty()) return;
    exprs_.kill(write, alias_);
    forwardStore(inst, write);
}

void ValueWalk::forwardStore(const il::Instruction& inst, const Access& write) {
    const ValueId v = storedValue(inst);
    if (v == kNoValue || write.where.place == Place::None) return;
    // What a later read of the stored place will see: the stored value, until
    // a write kills the entry exactly as it would kill a read's.
    Access read = write;
    read.immutable = false;
    const il::Type t = defs_.type(v);
    switch (inst.op) {
        case Op::SlotStore:
            exprs_.add(loadKey(Op::SlotLoad, t, write.where), v, read, true);
            return;
        case Op::ElemStore:
            exprs_.add(loadKey(Op::ElemLoad, t, write.where), v, read, true);
            return;
        // `env.set` stores a binding's value, never the uninitialized marker
        // (only `env.init.tdz` writes that), so it answers the checked read
        // too.
        case Op::EnvSet:
            exprs_.add(loadKey(Op::EnvGet, t, write.where), v, read, true);
            exprs_.add(loadKey(Op::EnvGetTdz, t, write.where), v, read, true);
            return;
        case Op::ModuleEnvSet:
            exprs_.add(loadKey(Op::ModuleEnvGet, t, write.where), v, read, true);
            return;
        case Op::ShapeStore:
        case Op::CreateThis:
            exprs_.add(loadKey(Op::LoadShape, il::Type::Ptr, write.where), v, read, true);
            return;
        default:
            return;
    }
}

bool ValueWalk::apply() {
    for (const auto& [b, taken] : branchFolds_) {
        il::Instruction& term = fn_.blocks[b].instructions.back();
        assert(term.op == Op::Branch);
        il::Instruction jump;
        jump.op = Op::Jump;
        jump.span = term.span;
        jump.target = taken ? std::move(term.target) : std::move(term.elseTarget);
        term = std::move(jump);
        ++ctx_.stats.branchesFolded;
        changed_ = true;
    }
    replaceAllUses(fn_, repl_);
    for (size_t v = 0; v < repl_.size(); ++v) {
        if (repl_[v] != kNoValue) ctx_.noteReplaced(static_cast<ValueId>(v), repl_[v]);
    }
    for (size_t b = 0; b < fn_.blocks.size(); ++b) {
        if (dead_[b].empty()) continue;
        auto& insts = fn_.blocks[b].instructions;
        std::vector<il::Instruction> kept;
        kept.reserve(insts.size());
        for (size_t i = 0; i < insts.size(); ++i) {
            if (!dead_[b][i]) kept.push_back(std::move(insts[i]));
        }
        insts = std::move(kept);
    }
    const size_t removed = removeUnreachableBlocks(fn_);
    ctx_.stats.blocksRemoved += removed;
    return changed_ || removed != 0;
}

}  // namespace

bool runValueWalk(JobContext& ctx) {
    il::Function& fn = ctx.fn;
    if (fn.blocks.empty()) return false;
    const Cfg cfg(fn);
    // A block holding a resume point of G is also entered by a resumption,
    // with only the point's state in hand: what is computed before it, or on
    // a path that does not run through it, is not available after it. Walked
    // as the root of a tree of its own, the block and everything only it
    // dominates start with an empty table; the point itself hides what the
    // block computed in front of it (visitInstruction). A use the walk makes
    // after a point is thus always of a value computed after it or of one in
    // its state, and G's points need no state but the one they were made
    // with.
    std::vector<BlockId> resumed;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        for (const il::Instruction& inst : fn.blocks[b].instructions) {
            if (inst.op == Op::ResumePoint) {
                resumed.push_back(static_cast<BlockId>(b));
                break;
            }
        }
    }
    const DomTree dom(cfg, resumed);
    const DefUse defs(fn);
    const AliasInfo alias(fn);
    FactEnv facts;
    ExprTable exprs;
    ValueWalk walk(ctx, defs, alias, facts, exprs);
    walkDominatorTree(fn, cfg, dom, defs, alias, facts, exprs, [&](BlockId b) { walk.visitBlock(b); });
    return walk.apply();
}

}  // namespace bronze::opt::redundancy
