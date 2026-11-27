// Partial escape analysis (exit-only allocation sinking):
//
// An unescaped fresh allocation whose every use on hot paths is a private store
// (`slot.store`, `shape.store`, `elem.store` with operand 0 == the object) and
// whose remaining uses lie strictly inside exit-only (`leave`) blocks can be
// deleted from the hot path and materialized lazily inside each exit block.
// Each exit receives its own allocation clone replayed with the dominating
// private stores in dominance order before its first use in that exit block.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

#include "il/effects.h"
#include "opt/core/cfg.h"
#include "opt/core/loops.h"
#include "opt/core/uses.h"
#include "opt/redundancy/location.h"
#include "opt/repr/phases.h"
#include "opt/repr/rewrite.h"

namespace bronze::opt::repr {

namespace {

using il::Op;

constexpr uint32_t kMaxAllocSinkRounds = 4;

std::vector<uint8_t> exitOnlyBlocks(const il::Function& fn, const Cfg& cfg) {
    std::vector<uint8_t> exit(fn.blocks.size(), 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (BlockId b : cfg.postorder()) {
            if (exit[b]) continue;
            const il::Block& block = fn.blocks[b];
            if (block.instructions.empty()) continue;
            const il::Instruction& term = block.instructions.back();
            bool now = term.op == Op::Leave;
            if (!now && block.cold && (term.op == Op::Jump || term.op == Op::Branch)) {
                const auto& s = cfg.succs(b);
                now = !s.empty() && std::all_of(s.begin(), s.end(), [&](BlockId succ) { return exit[succ] != 0; });
            }
            if (now) {
                exit[b] = 1;
                changed = true;
            }
        }
    }
    return exit;
}

bool isSinkableAlloc(const il::Instruction& inst) {
    switch (inst.op) {
        case Op::CreateThis:
        case Op::CreateObject:
        case Op::ObjectLiteral:
        case Op::CreateArray:
        case Op::ArrayLiteral: {
            const il::Effects e = il::effectsOf(inst);
            return !e.throws && !e.callsUnknown && !e.observableOrder && !e.barrier && e.reads.empty();
        }
        default:
            return false;
    }
}

bool isPrivateStore(const il::Instruction& inst) {
    if (inst.op != Op::SlotStore && inst.op != Op::ShapeStore && inst.op != Op::ElemStore) return false;
    if (inst.operands.empty()) return false;
    const il::Effects e = il::effectsOf(inst);
    return !e.throws && !e.callsUnknown && !e.observableOrder && !e.barrier && !e.allocates && !e.collects;
}

bool reachableWithout(const Cfg& cfg, BlockId start, BlockId target, BlockId barrier) {
    if (start == target) return true;
    std::vector<uint8_t> visited(cfg.size(), 0);
    std::vector<BlockId> queue;
    if (start != barrier) {
        visited[start] = 1;
        queue.push_back(start);
    }
    for (size_t q = 0; q < queue.size(); ++q) {
        for (BlockId succ : cfg.succs(queue[q])) {
            if (succ == barrier) continue;
            if (succ == target) return true;
            if (!visited[succ]) {
                visited[succ] = 1;
                queue.push_back(succ);
            }
        }
    }
    return false;
}

struct StoreSite {
    BlockId block = kNoBlock;
    uint32_t inst = 0;
};

class AllocSinker {
public:
    explicit AllocSinker(PhaseContext& ctx)
        : fn_(ctx.fn), cfg_(ctx.fn), dom_(cfg_), loops_(cfg_, dom_), defs_(ctx.fn), alias_(ctx.fn, &dom_),
          exit_(exitOnlyBlocks(ctx.fn, cfg_)) {}

    bool run();

private:
    bool trySink(BlockId allocBlock, size_t allocIdx, const il::Instruction& allocInst);

    il::Function& fn_;
    const Cfg cfg_;
    const DomTree dom_;
    const LoopForest loops_;
    const DefUse defs_;
    const redundancy::AliasInfo alias_;
    const std::vector<uint8_t> exit_;
    std::vector<uint8_t> scheduled_;
    EditPlan edits_;
    std::vector<std::pair<UseSite, ValueId>> rewrites_;
};

bool AllocSinker::trySink(BlockId allocBlock, size_t allocIdx, const il::Instruction& allocInst) {
    const ValueId v = allocInst.result;
    if (v == kNoValue || v >= fn_.valueCount || !isSinkableAlloc(allocInst) || !alias_.isFreshUnescaped(v)) {
        return false;
    }
    if (scheduled_[v]) return false;

    const std::vector<UseSite>& uses = defs_.uses(v);
    if (uses.empty()) return false;

    for (ValueId opd : allocInst.operands) {
        if (opd < scheduled_.size() && scheduled_[opd]) return false;
    }

    const uint32_t allocLoop = loops_.innermost(allocBlock);
    std::vector<StoreSite> stores;
    std::vector<BlockId> exitBlocks;
    for (const UseSite& u : uses) {
        if (!cfg_.reachable(u.block)) return false;
        if (exit_[u.block]) {
            if (!valueReaches(cfg_, dom_, allocBlock, u.block) || allocBlock == u.block) return false;
            if (std::find(exitBlocks.begin(), exitBlocks.end(), u.block) == exitBlocks.end()) {
                exitBlocks.push_back(u.block);
            }
            continue;
        }
        if (u.kind != UseSite::Kind::Operand || u.slot != 0) return false;
        const il::Instruction& sInst = fn_.blocks[u.block].instructions[u.inst];
        if (!isPrivateStore(sInst)) return false;
        if (!valueReaches(cfg_, dom_, allocBlock, u.block)) return false;
        if (u.block == allocBlock && u.inst <= allocIdx) return false;
        if (loops_.innermost(u.block) != allocLoop) return false;
        for (size_t k = 1; k < sInst.operands.size(); ++k) {
            const ValueId opd = sInst.operands[k];
            if (opd < scheduled_.size() && scheduled_[opd]) return false;
        }
        stores.push_back(StoreSite{u.block, u.inst});
    }

    if (exitBlocks.empty()) return false;
    std::sort(exitBlocks.begin(), exitBlocks.end());

    // Every non-exit store must either run on all paths from allocBlock to an
    // exit block or on no path to that exit block.
    for (const StoreSite& s : stores) {
        for (BlockId e : exitBlocks) {
            if (dom_.dominates(s.block, e)) {
                if (!valueReaches(cfg_, dom_, s.block, e)) return false;
                continue;
            }
            for (BlockId succ : cfg_.succs(s.block)) {
                if (reachableWithout(cfg_, succ, e, allocBlock)) return false;
            }
        }
    }

    scheduled_[v] = 1;
    for (BlockId e : exitBlocks) {
        uint32_t firstUseInst = UINT32_MAX;
        for (const UseSite& u : uses) {
            if (u.block == e && u.inst < firstUseInst) firstUseInst = u.inst;
        }
        assert(firstUseInst != UINT32_MAX);

        std::vector<StoreSite> domStores;
        for (const StoreSite& s : stores) {
            if (dom_.dominates(s.block, e)) domStores.push_back(s);
        }
        std::sort(domStores.begin(), domStores.end(), [&](const StoreSite& a, const StoreSite& b) {
            if (dom_.depth(a.block) != dom_.depth(b.block)) return dom_.depth(a.block) < dom_.depth(b.block);
            if (a.block != b.block) return a.block < b.block;
            return a.inst < b.inst;
        });

        const ValueId copyV = newValue(fn_);
        il::Instruction allocClone = allocInst;
        allocClone.result = copyV;
        edits_.insertBefore(e, firstUseInst, std::move(allocClone));

        for (const StoreSite& s : domStores) {
            il::Instruction storeClone = fn_.blocks[s.block].instructions[s.inst];
            storeClone.operands[0] = copyV;
            storeClone.result = kNoValue;
            edits_.insertBefore(e, firstUseInst, std::move(storeClone));
        }
        for (const UseSite& u : uses) {
            if (u.block == e) rewrites_.emplace_back(u, copyV);
        }
    }

    edits_.remove(allocBlock, allocIdx);
    for (const StoreSite& s : stores) edits_.remove(s.block, s.inst);
    return true;
}

bool AllocSinker::run() {
    scheduled_.assign(fn_.valueCount, 0);
    bool changed = false;
    for (BlockId b : cfg_.rpo()) {
        if (exit_[b]) continue;
        const auto& insts = fn_.blocks[b].instructions;
        for (size_t i = 0; i < insts.size(); ++i) {
            changed |= trySink(b, i, insts[i]);
        }
    }
    if (!changed) return false;
    for (const auto& [use, value] : rewrites_) useRef(fn_, use) = value;
    edits_.apply(fn_);
    return true;
}

}  // namespace

bool sinkAllocations(PhaseContext& ctx) {
    if (ctx.fn.blocks.empty()) return false;
    bool changed = false;
    for (uint32_t round = 0; round < kMaxAllocSinkRounds; ++round) {
        AllocSinker sinker(ctx);
        if (!sinker.run()) break;
        changed = true;
    }
    return changed;
}

}  // namespace bronze::opt::repr
