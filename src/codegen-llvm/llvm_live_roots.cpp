#include "codegen-llvm/llvm_live_roots.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

namespace bronze::codegen_llvm {

namespace {

// One value per bit, sized to the function. A function's value count is the
// width of every set here, and the sets are unioned block by block, so words
// rather than bytes: the three.js graph has functions with four thousand
// values and two hundred blocks, and a byte per value per block is a
// megabyte of memcpy per fixpoint round.
class BitSet {
public:
    void init(size_t bits) { words_.assign((bits + 63) / 64, 0); }
    bool test(uint32_t i) const { return ((words_[i >> 6] >> (i & 63)) & 1U) != 0; }
    void set(uint32_t i) { words_[i >> 6] |= 1ULL << (i & 63); }
    void reset(uint32_t i) { words_[i >> 6] &= ~(1ULL << (i & 63)); }
    void clear() { std::fill(words_.begin(), words_.end(), 0); }
    void unionWith(const BitSet& other) {
        for (size_t k = 0; k < words_.size(); ++k) words_[k] |= other.words_[k];
    }
    bool operator==(const BitSet& other) const { return words_ == other.words_; }
    // Every set bit, in value order, so an output derived from one is a
    // function of the IL and not of a traversal.
    template <typename Fn>
    void forEach(Fn&& fn) const {
        for (size_t w = 0; w < words_.size(); ++w) {
            uint64_t bits = words_[w];
            while (bits != 0) {
                const int idx = std::countr_zero(bits);
                fn(static_cast<uint32_t>(w * 64 + static_cast<size_t>(idx)));
                bits &= bits - 1;
            }
        }
    }

private:
    std::vector<uint64_t> words_;
};

// Every value an instruction READS, in the order the emitter reloads them:
// operands, then the two block-argument lists a terminator carries. The
// argument lists are read at the branch, so they are ordinary uses there —
// but they live on `inst.target`, not in `inst.operands`, and a walk that
// forgot them would call a value dead while a branch still needs it.
template <typename Fn>
void forEachUse(const il::Instruction& inst, Fn&& fn) {
    for (il::ValueId u : inst.operands) fn(u);
    for (il::ValueId u : inst.target.args) fn(u);
    for (il::ValueId u : inst.elseTarget.args) fn(u);
}

size_t useCountOf(const il::Instruction& inst) {
    return inst.operands.size() + inst.target.args.size() + inst.elseTarget.args.size();
}

// Whether this instruction can hand control to its block's handler.
//
// `il::canThrow` answers it for everything that raises through the pending
// cell, and two ops raise without going through one. `Op::Throw` writes the
// cell and branches itself, and `Op::PinGuard`'s branching form has its own
// violating arm (llvm_pin.h) — which is exactly why that form answers no to
// both effect predicates, and exactly why the arm's TypeError still has to
// find the handler's live values in their slots.
bool raisesToHandler(const il::Instruction& inst) {
    if (inst.op == il::Op::Throw) return true;
    if (inst.op == il::Op::PinGuard) {
        return static_cast<il::PinBarrier>(inst.immI32) != il::PinBarrier::DenseArray;
    }
    return il::canThrow(inst);
}

}  // namespace

bool liveRootsDisabled() {
    static const bool off = [] {
        const char* env = std::getenv("BRONZE_NO_LIVE_ROOTS");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return off;
}

LiveRootPlan planLiveRoots(const il::Function& func) {
    LiveRootPlan plan;
    const uint32_t n = func.valueCount;
    const size_t blockCount = func.blocks.size();

    // The use index, laid out first so both the seam's answer and the real one
    // are addressed the same way.
    plan.blockBase.assign(blockCount + 1, 0);
    size_t totalInsts = 0;
    for (size_t b = 0; b < blockCount; ++b) {
        plan.blockBase[b] = static_cast<uint32_t>(totalInsts);
        totalInsts += func.blocks[b].instructions.size();
    }
    plan.blockBase[blockCount] = static_cast<uint32_t>(totalInsts);
    plan.useBase.assign(totalInsts + 1, 0);
    size_t totalUses = 0;
    for (size_t b = 0; b < blockCount; ++b) {
        for (size_t i = 0; i < func.blocks[b].instructions.size(); ++i) {
            plan.useBase[plan.blockBase[b] + i] = static_cast<uint32_t>(totalUses);
            totalUses += useCountOf(func.blocks[b].instructions[i]);
        }
    }
    plan.useBase[totalInsts] = static_cast<uint32_t>(totalUses);
    plan.useAnchor.assign(totalUses, LiveRootPlan::kReload);

    if (liveRootsDisabled()) {
        plan.seamOff = true;
        plan.needsSlot.assign(n, 1);
        return plan;
    }
    plan.needsSlot.assign(n, 0);
    if (blockCount == 0) return plan;

    // ---- the CFG, exception edges included ------------------------------
    //
    // A handler is a successor of every block that can raise into it, and the
    // raise is what a value live in the handler has to survive. It is not a
    // terminator edge, so it is carried separately as well: a block a handler
    // edge reaches is entered from an arbitrary point inside its predecessor,
    // and the register state there is nobody's block exit.
    std::vector<std::vector<uint32_t>> succs(blockCount);
    std::vector<uint32_t> handlerOf(blockCount, il::kNoBlock);
    std::vector<uint32_t> normalPreds(blockCount, 0);
    std::vector<uint32_t> excPreds(blockCount, 0);
    std::vector<uint32_t> soleNormalPred(blockCount, il::kNoBlock);

    for (uint32_t b = 0; b < blockCount; ++b) {
        const il::Block& block = func.blocks[b];
        bool raises = false;
        for (const il::Instruction& inst : block.instructions) {
            if (raisesToHandler(inst)) raises = true;
            if (!il::isTerminator(inst.op)) continue;
            for (const il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                if (t->block == il::kNoBlock || t->block >= blockCount) continue;
                succs[b].push_back(t->block);
                ++normalPreds[t->block];
                soleNormalPred[t->block] = b;
            }
        }
        if (raises && block.handler != il::kNoBlock && block.handler < blockCount) {
            handlerOf[b] = block.handler;
            succs[b].push_back(block.handler);
            ++excPreds[block.handler];
        }
    }

    // ---- liveness, backwards to a fixpoint -------------------------------
    std::vector<BitSet> liveIn(blockCount);
    std::vector<BitSet> liveOut(blockCount);
    for (size_t b = 0; b < blockCount; ++b) {
        liveIn[b].init(n);
        liveOut[b].init(n);
    }
    BitSet live;
    live.init(n);
    BitSet out;
    out.init(n);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t bi = blockCount; bi-- > 0;) {
            const il::Block& block = func.blocks[bi];
            out.clear();
            for (uint32_t s : succs[bi]) out.unionWith(liveIn[s]);
            live = out;
            for (size_t i = block.instructions.size(); i-- > 0;) {
                const il::Instruction& inst = block.instructions[i];
                // The handler edge leaves AFTER the result store, which is
                // where the emitter puts the pending-cell test, so the
                // handler's live set joins before the def kills.
                if (handlerOf[bi] != il::kNoBlock && raisesToHandler(inst)) {
                    live.unionWith(liveIn[handlerOf[bi]]);
                }
                if (inst.result != il::kNoValue && inst.result < n) live.reset(inst.result);
                forEachUse(inst, [&](il::ValueId u) {
                    if (u != il::kNoValue && u < n) live.set(u);
                });
            }
            for (const il::BlockParam& p : block.params) {
                if (p.id != il::kNoValue && p.id < n) live.reset(p.id);
            }
            liveOut[bi] = out;
            if (!(liveIn[bi] == live)) {
                liveIn[bi] = live;
                changed = true;
            }
        }
    }

    // ---- which values a collection can see -------------------------------
    //
    // A value needs a slot exactly where a collection can happen while it is
    // still wanted. That is the set live IMMEDIATELY BEFORE each collecting
    // instruction — which contains the instruction's own operands, because a
    // helper holds them in registers of its own while it allocates, and the
    // slot is what the collector forwards on its behalf.
    for (size_t bi = 0; bi < blockCount; ++bi) {
        const il::Block& block = func.blocks[bi];
        if (handlerOf[bi] != il::kNoBlock) {
            // Everything the handler can read is reached through a raise, and a
            // raise mints its error object. That covers the pin barrier's
            // violating arm too, which mints a TypeError on its way out and
            // never comes back — so nothing on the kept path is stale after the
            // guard, and the handler's values still have to be in their slots
            // in front of it.
            liveIn[handlerOf[bi]].forEach([&](uint32_t v) { plan.needsSlot[v] = 1; });
        }
        live = liveOut[bi];
        for (size_t i = block.instructions.size(); i-- > 0;) {
            const il::Instruction& inst = block.instructions[i];
            if (handlerOf[bi] != il::kNoBlock && raisesToHandler(inst)) {
                live.unionWith(liveIn[handlerOf[bi]]);
            }
            if (inst.result != il::kNoValue && inst.result < n) live.reset(inst.result);
            forEachUse(inst, [&](il::ValueId u) {
                if (u != il::kNoValue && u < n) live.set(u);
            });
            if (il::canCollect(inst)) {
                live.forEach([&](uint32_t v) { plan.needsSlot[v] = 1; });
            }
        }
    }

    // ---- where a use may read its register --------------------------------
    //
    // Forwards, in emission order. `anchor[v]` is the block whose emission last
    // wrote v's register; `kReload` says nothing trustworthy is in one. A
    // collecting instruction invalidates every register at once, because the
    // collector forwards slots and not registers.
    constexpr uint32_t kNone = LiveRootPlan::kReload;
    std::vector<std::vector<uint32_t>> pendingEntry(blockCount);
    std::vector<uint32_t> anchor;
    for (uint32_t b = 0; b < blockCount; ++b) {
        if (!pendingEntry[b].empty()) {
            anchor = std::move(pendingEntry[b]);
            pendingEntry[b] = std::vector<uint32_t>{};
        } else {
            anchor.assign(n, kNone);
            if (b == 0) {
                // A function's parameters arrive in registers and the prologue
                // stores them, so they are current at entry — except in the
                // entry point, whose block 0 carries the module's registration
                // calls in front of its own first instruction, and those
                // allocate.
                if (!func.isEntryPoint) {
                    for (size_t p = 0; p < func.params.size() && p < n; ++p) {
                        anchor[p] = 0;
                    }
                }
            }
        }
        const il::Block& block = func.blocks[b];
        // A block parameter is a phi at the block's own entry: it dominates
        // everything in the block, and its incomings are the registers each
        // predecessor's branch had just made current.
        for (const il::BlockParam& p : block.params) {
            if (p.id != il::kNoValue && p.id < n) anchor[p.id] = b;
        }
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const il::Instruction& inst = block.instructions[i];
            uint32_t at = plan.useBase[plan.blockBase[b] + i];
            forEachUse(inst, [&](il::ValueId u) {
                uint32_t answer = kNone;
                if (u != il::kNoValue && u < n && plan.needsSlot[u] != 0) {
                    if (anchor[u] != kNone) {
                        answer = anchor[u];
                    } else {
                        anchor[u] = b;  // this use reloads, so the register is current after it
                    }
                }
                plan.useAnchor[at++] = answer;
            });
            if (il::canCollect(inst)) std::fill(anchor.begin(), anchor.end(), kNone);
            if (inst.result != il::kNoValue && inst.result < n && plan.needsSlot[inst.result] != 0) {
                anchor[inst.result] = b;
            }
        }
        // A successor this block is the ONLY way into carries the registers
        // forward: one predecessor means this block dominates it, and a
        // dominating block's registers are legal to use there. A successor a
        // handler edge also reaches is not such a block, whatever its
        // terminator count says.
        for (uint32_t s : succs[b]) {
            if (s <= b || excPreds[s] != 0 || normalPreds[s] != 1) continue;
            if (soleNormalPred[s] != b) continue;
            pendingEntry[s] = anchor;
        }
    }

    return plan;
}

std::vector<LiveRootPlan> planLiveRoots(const il::Module& module) {
    std::vector<LiveRootPlan> plans;
    plans.reserve(module.functions.size());
    for (const il::Function& func : module.functions) plans.push_back(planLiveRoots(func));
    return plans;
}

}  // namespace bronze::codegen_llvm
