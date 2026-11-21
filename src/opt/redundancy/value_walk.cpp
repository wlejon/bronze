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

#include <algorithm>
#include <cassert>
#include <utility>

#include "abi/bronze_abi_layout.h"
#include "il/effects.h"
#include "opt/core/cfg_edit.h"
#include "opt/core/fact_env.h"
#include "opt/core/uses.h"
#include "opt/redundancy/dom_walk.h"
#include "opt/redundancy/expr_table.h"
#include "opt/redundancy/fold.h"
#include "opt/redundancy/index_facts.h"
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
    ValueWalk(JobContext& ctx, const Cfg& cfg, const DefUse& defs, const AliasInfo& alias, FactEnv& facts,
              ExprTable& exprs, const IndexFacts& index)
        : ctx_(ctx),
          fn_(ctx.fn),
          cfg_(cfg),
          defs_(defs),
          values_(ctx.fn, defs),
          alias_(alias),
          facts_(facts),
          exprs_(exprs),
          index_(index),
          repl_(ctx.fn.valueCount, kNoValue),
          dead_(ctx.fn.blocks.size()),
          folded_(ctx.fn.blocks.size(), -1),
          deadBlock_(ctx.fn.blocks.size(), 0) {}

    void visitBlock(BlockId b);
    bool apply();
    // Whether the edge from -> to is never taken: the arm of a branch this
    // walk folded the other way, or out of a block only such edges reach.
    bool edgeDead(BlockId from, BlockId to) const;

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
    // A fresh literal's values answer the reads of its slots and elements.
    void forwardLiteral(const il::Instruction& inst);
    // Marks `start`, and what only it reaches, dead once every edge into it is.
    void markDead(BlockId start);

    JobContext& ctx_;
    il::Function& fn_;
    const Cfg& cfg_;
    const DefUse& defs_;
    ValueView values_;
    const AliasInfo& alias_;
    FactEnv& facts_;
    ExprTable& exprs_;
    const IndexFacts& index_;
    std::vector<ValueId> repl_;
    std::vector<std::vector<uint8_t>> dead_;
    std::vector<std::pair<BlockId, bool>> branchFolds_;
    std::vector<int8_t> folded_;  // per block: -1, or the arm its branch folded to
    std::vector<uint8_t> deadBlock_;
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
    // What `x < n` proves of a non-negative index (index_facts.h): scoped,
    // like the edge's own facts, to b's subtree. Named by the values standing
    // for x and n now, which the tests' resolved operands are.
    for (Fact fact : index_.onEntry(b)) {
        fact.value = resolve(fact.value);
        fact.other = resolve(fact.other);
        facts_.add(fact);
    }
    const size_t n = fn_.blocks[b].instructions.size();
    for (size_t i = 0; i < n; ++i) visitInstruction(b, i);
}

void ValueWalk::visitTerminator(BlockId b, const il::Instruction& inst) {
    if (inst.op != Op::Branch || inst.operands.empty()) return;
    const ValueId cond = inst.operands[0];
    std::optional<bool> known = constBool(values_, cond);
    if (!known) known = facts_.boolValue(cond);
    if (known) {
        branchFolds_.emplace_back(b, *known);
        folded_[b] = *known ? 1 : 0;
        markDead(*known ? inst.elseTarget.block : inst.target.block);
    }
}

bool ValueWalk::edgeDead(BlockId from, BlockId to) const {
    if (from >= folded_.size()) return false;
    if (deadBlock_[from]) return true;
    if (folded_[from] < 0) return false;
    const il::Instruction& term = fn_.blocks[from].instructions.back();
    const BlockId taken = folded_[from] == 1 ? term.target.block : term.elseTarget.block;
    return to != taken;
}

void ValueWalk::markDead(BlockId start) {
    std::vector<BlockId> work{start};
    while (!work.empty()) {
        const BlockId b = work.back();
        work.pop_back();
        if (b == 0 || b >= deadBlock_.size() || deadBlock_[b]) continue;
        bool live = false;
        for (BlockId p : cfg_.preds(b)) live = live || !edgeDead(p, b);
        if (live) continue;
        deadBlock_[b] = 1;
        for (BlockId s : cfg_.succs(b)) work.push_back(s);
    }
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
    } else if (const std::optional<bool> owns = knownHasOwnResult(inst, facts_, exprs_, values_, ctx_.module)) {
        makeBoolConstant(inst, *owns);
        ++ctx_.stats.testsFolded;
        changed_ = true;
    }

    // A store of a known Number keeps the array's NUMBERS bit: marked, its
    // effect no longer writes `ElemKinds`, so NUMBERS tests survive it.
    if (inst.op == Op::ElemStore && (inst.immI32 & il::kStoreNumber) == 0) {
        const ValueId stored = inst.operands.size() == 3u ? inst.operands.back() : kNoValue;
        if (stored != kNoValue && knownNumber(facts_, values_, stored)) {
            inst.immI32 |= il::kStoreNumber;
            changed_ = true;
        }
    }
    if (inst.op == Op::ArrayPush && (inst.immI32 & il::kStoreNumber) == 0 && inst.operands.size() >= 2) {
        bool allNumbers = true;
        for (size_t k = 1; k < inst.operands.size(); ++k) {
            if (!knownNumber(facts_, values_, inst.operands[k])) {
                allNumbers = false;
                break;
            }
        }
        if (allNumbers) {
            inst.immI32 |= il::kStoreNumber;
            changed_ = true;
        }
    }

    // Folding may have changed the op, so the effects are read afresh.
    const il::Effects e = il::effectsOf(inst);
    if (reusable(inst, e)) {
        if (lookupAndRecord(b, i, e)) return;
    } else {
        recordWrite(inst, e);
        forwardLiteral(inst);
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
        // A read in the Number form (a double slot's, a NUMBERS array's
        // element) is a key of its own; the stored value answers it too,
        // being what the place holds (a later read takes the value, whose
        // own definition says what it is).
        case Op::SlotStore: {
            exprs_.add(loadKey(Op::SlotLoad, t, write.where), v, read, true);
            ExprKey number = loadKey(Op::SlotLoad, t, write.where);
            number.boxType = il::Type::F64;
            exprs_.add(std::move(number), v, read, true);
            return;
        }
        case Op::ElemStore: {
            if (write.where.place != Place::Elem) return;
            exprs_.add(loadKey(Op::ElemLoad, t, write.where), v, read, true);
            ExprKey number = loadKey(Op::ElemLoad, t, write.where);
            number.immI32 = il::kElemLoadNumber;
            exprs_.add(std::move(number), v, read, true);
            return;
        }
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

// `object.literal` and `array.literal` make a fresh object whose layout the
// op fixes (il_op.h): key i of an object literal sits at the slot the runtime
// places the i-th add of a width-1 property at, from the root the literal's
// inline capacity picks (runtime/shape.h placeSlot; il2mir
// il_lowering_literal.cpp picks the same root), and element i of an array
// literal is %vi. So each value answers the read of its place exactly as a
// store there would (forwardStore), until a write kills it. A slot read is
// only ever behind a guard naming a shape off the literal's root (a
// dictionary object never passes one), so the slot numbering is the one
// those shapes have. An element read is keyed by its index VALUE, so only
// the indices an available i32 constant already names are registered.
void ValueWalk::forwardLiteral(const il::Instruction& inst) {
    if (inst.result == kNoValue) return;
    if (inst.op == Op::ObjectLiteral) {
        if (inst.immI32 < 0 || static_cast<size_t>(inst.immI32) >= ctx_.module.objectLiterals.size()) return;
        const il::Module::ObjectLiteral& lit = ctx_.module.objectLiterals[static_cast<size_t>(inst.immI32)];
        if (lit.keys.size() != inst.operands.size()) return;
        const uint32_t cap = lit.inlineSlots <= BRONZE_ABI_OBJ_INLINE_SLOTS
                                 ? BRONZE_ABI_OBJ_INLINE_SLOTS
                                 : std::min<uint32_t>(lit.inlineSlots, BRONZE_ABI_OBJ_OVERFLOW_SLOT);
        for (uint32_t i = 0; i < inst.operands.size(); ++i) {
            Location where;
            where.place = Place::Slot;
            where.base = inst.result;
            where.a = i < cap ? i : BRONZE_ABI_OBJ_OVERFLOW_SLOT + (i - cap);
            Access read;
            read.locs = il::Loc::Slots;
            read.where = where;
            const ValueId v = inst.operands[i];
            exprs_.add(loadKey(Op::SlotLoad, defs_.type(v), where), v, read, true);
            ExprKey number = loadKey(Op::SlotLoad, defs_.type(v), where);
            number.boxType = il::Type::F64;
            exprs_.add(std::move(number), v, read, true);
        }
        return;
    }
    if (inst.op != Op::ArrayLiteral) return;
    for (uint32_t i = 0; i < inst.operands.size(); ++i) {
        il::Instruction c;
        c.op = Op::ConstI32;
        c.type = il::Type::I32;
        c.immI32 = static_cast<int32_t>(i);
        const ValueId index = exprs_.lookup(exprKeyOf(c)).value;
        if (index == kNoValue) continue;
        Location where;
        where.place = Place::Elem;
        where.base = inst.result;
        where.index = index;
        Access read;
        read.locs = il::Loc::Elements;
        read.where = where;
        const ValueId v = inst.operands[i];
        exprs_.add(loadKey(Op::ElemLoad, defs_.type(v), where), v, read, true);
        ExprKey number = loadKey(Op::ElemLoad, defs_.type(v), where);
        number.immI32 = il::kElemLoadNumber;
        exprs_.add(std::move(number), v, read, true);
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
    const AliasInfo alias(fn, &dom);
    const IndexFacts index(fn, cfg, dom, defs);
    FactEnv facts;
    ExprTable exprs;
    ValueWalk walk(ctx, cfg, defs, alias, facts, exprs, index);
    walkDominatorTree(
        fn, cfg, dom, defs, alias, facts, exprs, [&](BlockId b) { walk.visitBlock(b); },
        [&](BlockId from, BlockId to) { return walk.edgeDead(from, to); });
    return walk.apply();
}

}  // namespace bronze::opt::redundancy
