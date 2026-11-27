// Job 5: DEAD STORE ELIMINATION, block-local.
//
// A keyed store (`slot.store`, `elem.store`, `env.set`, `env.init.tdz`,
// `module.env.set`, `shape.store`) is dead when a later store in the same block writes the
// same place (location.h sameLocation: same base VALUE, same slot / index /
// depth) and nothing between the two can observe the first:
//   - no instruction whose read may overlap the place (mayOverlap, which
//     ignores immutability: the read of an immutable binding still sees the
//     store that initialized it);
//   - no instruction that can throw: the handler, or the caller the throw
//     reaches, may read the heap the first store left;
//   - no barrier (a resume point reads everything anyway);
//   - for a shape word, no collection point: the collector reads it;
//   - and the second store is in the same block, so no terminator lies
//     between them — a `leave`, a `ret` or a branch to an exit block is a way
//     out that would read everything (G resumes and reads the heap).
// `callsUnknown` reads everything, so the read test covers it.
//
// Deliberately no more than that: stores across blocks need post-dominance
// and the exits of S make almost every path observable.

#include <bit>
#include <vector>

#include "il/effects.h"
#include "opt/redundancy/jobs.h"
#include "opt/redundancy/location.h"

namespace bronze::opt::redundancy {

namespace {

// Whether store `i` of `insts` is overwritten, unobserved, later in the block.
bool overwrittenBeforeRead(const std::vector<il::Instruction>& insts, size_t i, const Access& store,
                           const AliasInfo& alias) {
    for (size_t j = i + 1; j < insts.size(); ++j) {
        const il::Instruction& next = insts[j];
        if (il::isTerminator(next.op)) return false;
        const il::Effects e = il::effectsOf(next);
        if (e.throws || e.barrier || e.callsUnknown) return false;
        // The collector reads an object's shape word to find its slots.
        if (e.collects && store.where.place == Place::Shape) return false;
        if (mayOverlap(store, readOf(next, e), alias)) return false;
        const Access write = writeOf(next, e);
        if (write.where.place != Place::None && sameLocation(write.where, store.where, alias)) return true;
    }
    return false;
}

bool isDeadRemovableAlloc(il::Op op) {
    return op == il::Op::CreateThis || op == il::Op::CreateObject || op == il::Op::CreateArray ||
           op == il::Op::ObjectLiteral || op == il::Op::ArrayLiteral;
}

bool isSelfPrivateStore(const il::Instruction& inst) {
    if (inst.op != il::Op::SlotStore && inst.op != il::Op::ShapeStore && inst.op != il::Op::ElemStore) {
        return false;
    }
    if (inst.operands.empty()) return false;
    const il::Effects e = il::effectsOf(inst);
    return !e.throws && !e.callsUnknown && !e.observableOrder && !e.barrier && !e.allocates && !e.collects;
}

bool eliminateDeadAllocations(JobContext& ctx, const AliasInfo& alias) {
    il::Function& fn = ctx.fn;
    const size_t n = fn.valueCount;
    if (n == 0) return false;
    std::vector<uint8_t> used(n, 0);
    auto mark = [&](ValueId v) {
        if (v != kNoValue && v < n) used[v] = 1;
    };
    for (const il::Block& block : fn.blocks) {
        for (const il::Instruction& inst : block.instructions) {
            for (ValueId v : inst.target.args) mark(v);
            for (ValueId v : inst.elseTarget.args) mark(v);
            const bool selfPrivate = isSelfPrivateStore(inst);
            for (size_t i = 0; i < inst.operands.size(); ++i) {
                if (i == 0 && selfPrivate) continue;
                mark(inst.operands[i]);
            }
        }
    }

    std::vector<uint8_t> deadAlloc(n, 0);
    bool anyDead = false;
    for (const il::Block& block : fn.blocks) {
        for (const il::Instruction& inst : block.instructions) {
            if (inst.result != kNoValue && inst.result < n && isDeadRemovableAlloc(inst.op) &&
                alias.isFreshUnescaped(inst.result) && !used[inst.result]) {
                deadAlloc[inst.result] = 1;
                anyDead = true;
            }
        }
    }
    if (!anyDead) return false;

    for (il::Block& block : fn.blocks) {
        auto& insts = block.instructions;
        std::vector<il::Instruction> kept;
        kept.reserve(insts.size());
        for (il::Instruction& inst : insts) {
            if (inst.result != kNoValue && inst.result < n && deadAlloc[inst.result]) {
                ++ctx.stats.deadInstructions;
                continue;
            }
            if (!inst.operands.empty() && inst.operands[0] != kNoValue && inst.operands[0] < n &&
                deadAlloc[inst.operands[0]] && isSelfPrivateStore(inst)) {
                ++ctx.stats.deadStores;
                continue;
            }
            kept.push_back(std::move(inst));
        }
        insts = std::move(kept);
    }
    return true;
}

}  // namespace

bool runDeadStores(JobContext& ctx) {
    il::Function& fn = ctx.fn;
    if (fn.blocks.empty()) return false;
    const AliasInfo alias(fn);
    bool changed = false;
    for (il::Block& block : fn.blocks) {
        auto& insts = block.instructions;
        std::vector<uint8_t> dead(insts.size(), 0);
        bool any = false;
        for (size_t i = 0; i < insts.size(); ++i) {
            const il::Effects e = il::effectsOf(insts[i]);
            // Only a store whose ONLY effect is its keyed write: one that also
            // throws, calls or orders against output is kept whatever follows.
            if (e.throws || e.callsUnknown || e.observableOrder || e.barrier || e.allocates) continue;
            const Access store = writeOf(insts[i], e);
            // The place covers the whole write only when it is the write's one
            // location class; a store touching more is not this job's.
            if (store.where.place == Place::None || std::popcount(store.locs.bits()) != 1) continue;
            if (overwrittenBeforeRead(insts, i, store, alias)) {
                dead[i] = 1;
                any = true;
            }
        }
        if (!any) continue;
        std::vector<il::Instruction> kept;
        kept.reserve(insts.size());
        for (size_t i = 0; i < insts.size(); ++i) {
            if (dead[i]) {
                ++ctx.stats.deadStores;
            } else {
                kept.push_back(std::move(insts[i]));
            }
        }
        insts = std::move(kept);
        changed = true;
    }
    changed |= eliminateDeadAllocations(ctx, alias);
    return changed;
}

}  // namespace bronze::opt::redundancy
