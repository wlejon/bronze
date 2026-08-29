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
    return planLiveRoots(func, RunArmPlan{});
}

LiveRootPlan planLiveRoots(const il::Function& func, RunArmPlan arms) {
    LiveRootPlan plan;
    plan.arms = std::move(arms);
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
        // The seam is the contract that stood before this stage, and the arms
        // are part of what this stage buys — a group whose join no anchor
        // trusts is a branch that saves nothing — so it takes them with it.
        plan.arms = RunArmPlan{};
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
    // What is live the instant a run-arm group finishes, per group. The join
    // has to hand every one of these forward — the fast arm's register on one
    // edge, the slow arm's reload on the other — and this backward walk is
    // already standing exactly where the answer is.
    std::vector<std::vector<il::ValueId>> liveAfterGroup(plan.arms.groups.size());

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
            // `live` here is what is live AFTER this instruction, which for a
            // group's last member is what its join owes the rest of the block.
            const uint32_t closing = plan.arms.memberAt(bi, i);
            if (closing != RunArmPlan::kNoGroup && plan.arms.groups[closing].last == i) {
                live.forEach([&](uint32_t v) { liveAfterGroup[closing].push_back(v); });
            }
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
    const bool meetPreds = !runArmsDisabled();

    // The normal predecessors of each block, which is `succs` read the other way
    // round minus the handler edges — those are carried in `excPreds` and end a
    // chain wherever they land.
    std::vector<std::vector<uint32_t>> preds(blockCount);
    for (uint32_t b = 0; b < blockCount; ++b) {
        for (const il::Instruction& inst : func.blocks[b].instructions) {
            if (!il::isTerminator(inst.op)) continue;
            for (const il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                if (t->block == il::kNoBlock || t->block >= blockCount) continue;
                preds[t->block].push_back(b);
            }
        }
    }

    // What each block leaves for its successors, kept only for the values that
    // are live out of it and have a slot — a block's exit map is otherwise the
    // whole value space and the meet below would be a walk over it per edge.
    // Value order, because `BitSet::forEach` gives value order, which is what
    // lets the intersection be a merge.
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> anchorOut(blockCount);
    std::vector<std::pair<uint32_t, uint32_t>> meet;
    std::vector<std::pair<uint32_t, uint32_t>> next;
    std::vector<uint32_t> anchor;

    // WHICH BLOCK'S EMISSION STILL HOLDS THE REGISTER. The meet above is a
    // statement about the CFG: on every path to this block the last write of v
    // was in block X, so X dominates here and X's register is legal. The
    // EMITTER, though, keeps one register per value and walks blocks in INDEX
    // order — and index order is not the CFG's order. A guarded region emits its
    // slow copy as a low-numbered block reachable only from the bottom of the
    // fast copy (src/lower/guard_region.h), so that block's own reloads run
    // BETWEEN X and here and leave the emitter's `regBlock_` naming a block that
    // dominates nothing on this path. `FunctionEmitter::reload` catches that per
    // use and goes to the slot, but a run-arm group's join hands its restore
    // registers straight to a phi and to the slow arm's spill with no such
    // fallback, and its results' slots are elided on the strength of no use ever
    // reloading. Both need the anchor to be one the emitter still HOLDS, so this
    // mirrors `regBlock_` across the whole walk and the meet keeps only the
    // entries the two agree on.
    std::vector<uint32_t> written(n, kNone);
    for (size_t p = 0; p < func.params.size() && p < n; ++p) written[p] = 0;

    for (uint32_t b = 0; b < blockCount; ++b) {
        anchor.assign(n, kNone);
        if (b == 0) {
            // A function's parameters arrive in registers and the prologue
            // stores them, so they are current at entry — except in the
            // entry point, whose block 0 carries the module's registration
            // calls in front of its own first instruction, and those
            // allocate.
            if (!func.isEntryPoint) {
                for (size_t p = 0; p < func.params.size() && p < n; ++p) anchor[p] = 0;
            }
        } else if (excPreds[b] == 0 && !preds[b].empty()) {
            // Every predecessor has to have been walked already — a back edge
            // comes from a block whose exit map is not written yet, and a
            // register that survives a loop is not a claim this pass makes.
            bool usable = true;
            for (uint32_t p : preds[b]) {
                if (p >= b) usable = false;
            }
            // Under the seam the old rule stands: one predecessor and no other
            // way in. That is the single-element case of the meet, so the two
            // differ in this line and nowhere else.
            if (!meetPreds && (normalPreds[b] != 1 || soleNormalPred[b] >= b)) usable = false;
            if (usable) {
                meet = anchorOut[preds[b][0]];
                for (size_t k = 1; k < preds[b].size() && !meet.empty(); ++k) {
                    const auto& other = anchorOut[preds[b][k]];
                    next.clear();
                    size_t x = 0;
                    size_t y = 0;
                    while (x < meet.size() && y < other.size()) {
                        if (meet[x].first < other[y].first) {
                            ++x;
                        } else if (other[y].first < meet[x].first) {
                            ++y;
                        } else {
                            if (meet[x].second == other[y].second) next.push_back(meet[x]);
                            ++x;
                            ++y;
                        }
                    }
                    meet.swap(next);
                }
                for (const auto& entry : meet) {
                    if (written[entry.first] == entry.second) anchor[entry.first] = entry.second;
                }
            }
        }
        const il::Block& block = func.blocks[b];
        // A block parameter is a phi at the block's own entry: it dominates
        // everything in the block, and its incomings are the registers each
        // predecessor's branch had just made current.
        for (const il::BlockParam& p : block.params) {
            if (p.id != il::kNoValue && p.id < n) {
                anchor[p.id] = b;
                written[p.id] = b;
            }
        }
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const uint32_t opens = plan.arms.startAt(b, i);
            if (opens != RunArmPlan::kNoGroup) {
                // THE GROUP, as one instruction that does not collect. Its own
                // members read nothing out of a register — the slow arm
                // re-emits them behind ladders that collect, so every use
                // inside it reloads — and its join hands the rest of the block
                // both the results and everything that was live across it.
                RunArmGroup& group = plan.arms.groups[opens];
                for (uint32_t k = group.first; k <= group.last; ++k) {
                    const uint32_t base = plan.useBase[plan.blockBase[b] + k];
                    const uint32_t end = plan.useBase[plan.blockBase[b] + k + 1];
                    for (uint32_t at = base; at < end; ++at) plan.useAnchor[at] = kNone;
                }
                group.restore.clear();
                group.restoreAnchor.clear();
                for (il::ValueId v : liveAfterGroup[opens]) {
                    if (v >= n || plan.needsSlot[v] == 0 || anchor[v] == kNone) continue;
                    bool defined = false;
                    for (il::ValueId r : group.result) defined = defined || r == v;
                    if (!defined) {
                        group.restore.push_back(v);
                        group.restoreAnchor.push_back(anchor[v]);
                    }
                }
                for (il::ValueId r : group.result) {
                    if (r != il::kNoValue && r < n && plan.needsSlot[r] != 0) {
                        anchor[r] = b;
                        written[r] = b;
                    }
                }
                // The join phi'd every restored value too, and that phi is the
                // register the rest of the block reads — not the one the value
                // arrived in. Saying so is what keeps the plan and the emitter
                // holding the same register: without it a use after the group
                // would be sent to a slot the fast arm never wrote, because the
                // value's own group elided that store on the strength of no use
                // reloading (`armLocalSlot` below).
                for (il::ValueId v : group.restore) {
                    anchor[v] = b;
                    written[v] = b;
                }
                i = group.last;
                continue;
            }
            const il::Instruction& inst = block.instructions[i];
            uint32_t at = plan.useBase[plan.blockBase[b] + i];
            forEachUse(inst, [&](il::ValueId u) {
                uint32_t answer = kNone;
                if (u != il::kNoValue && u < n && plan.needsSlot[u] != 0) {
                    if (anchor[u] != kNone) {
                        answer = anchor[u];
                    } else {
                        anchor[u] = b;  // this use reloads, so the register is current after it
                        written[u] = b;
                    }
                }
                plan.useAnchor[at++] = answer;
            });
            // A collection forwards slots and not registers, so it ends every
            // anchor — but it writes no register, so it changes nothing about
            // WHICH block last wrote one.
            if (il::canCollect(inst)) std::fill(anchor.begin(), anchor.end(), kNone);
            if (inst.result != il::kNoValue && inst.result < n && plan.needsSlot[inst.result] != 0) {
                anchor[inst.result] = b;
                written[inst.result] = b;
            }
        }
        liveOut[b].forEach([&](uint32_t v) {
            if (plan.needsSlot[v] != 0 && anchor[v] != kNone) {
                anchorOut[b].emplace_back(v, anchor[v]);
            }
        });
    }

    // ---- whose slot the fast arm may leave alone --------------------------
    //
    // A group result whose slot NOTHING outside the group's own slow arm reads.
    // Three readers, and all three are asked about here rather than at the
    // emitter, because the emitter sees one instruction at a time and this is a
    // property of every use at once:
    //
    //   a reloading use     — the plan just answered `kReload` for it, so it
    //                         will go to the slot and the slot has to be there.
    //   a handler           — entered from an arbitrary point, reading whatever
    //                         its live values' slots hold.
    //   an access receiver  — llvm_ops_access.cpp hands the receiver's slot
    //                         ADDRESS to the static-slot publish, which re-reads
    //                         it after the helper that may have moved it.
    //
    // A LATER GROUP is not on that list, and deliberately. Two adjacent runs of
    // sixteen put the first run's results on the second's restore list, and the
    // second's slow arm reloads them out of slots the first's fast arm never
    // wrote — so that arm SPILLS them on its way in (llvm_run_arms.cpp) instead
    // of making the fast path write sixteen words for a path it does not take.
    if (!plan.arms.empty()) {
        std::vector<uint8_t> slotRead(n, 0);
        for (size_t b = 0; b < blockCount; ++b) {
            if (handlerOf[b] != il::kNoBlock) {
                liveIn[handlerOf[b]].forEach([&](uint32_t v) { slotRead[v] = 1; });
            }
            const il::Block& block = func.blocks[b];
            for (size_t i = 0; i < block.instructions.size(); ++i) {
                const il::Instruction& inst = block.instructions[i];
                if ((inst.op == il::Op::PropGet || inst.op == il::Op::PropSet) &&
                    !inst.operands.empty() && inst.operands[0] < n) {
                    slotRead[inst.operands[0]] = 1;
                }
                uint32_t at = plan.useBase[plan.blockBase[b] + i];
                forEachUse(inst, [&](il::ValueId u) {
                    if (u != il::kNoValue && u < n && plan.useAnchor[at] == kNone) slotRead[u] = 1;
                    ++at;
                });
            }
        }
        plan.armLocalSlot.assign(n, 0);
        for (const RunArmGroup& group : plan.arms.groups) {
            for (il::ValueId r : group.result) {
                if (r == il::kNoValue || r >= n) continue;
                if (plan.needsSlot[r] != 0 && slotRead[r] == 0) plan.armLocalSlot[r] = 1;
            }
        }
    }

    return plan;
}

std::vector<LiveRootPlan> planLiveRoots(const il::Module& module) {
    std::vector<LiveRootPlan> plans;
    plans.reserve(module.functions.size());
    for (const il::Function& func : module.functions) {
        plans.push_back(planLiveRoots(func, planRunArms(module, func)));
    }
    return plans;
}

}  // namespace bronze::codegen_llvm
