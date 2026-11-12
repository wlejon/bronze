#include "lower/loop_truth_hoist.h"

#include <utility>
#include <vector>

#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

bool isTruthTest(const il::Instruction& inst) {
    return inst.op == il::Op::Unbox && inst.type == il::Type::Bool && !inst.rawUnbox && !inst.nullishUnbox &&
           inst.operands.size() == 1 && inst.result != il::kNoValue;
}

// One loop's tests moved, or false when no loop had one to move.
bool hoistOne(il::Function& fn) {
    const Cfg cfg = buildCfg(fn);
    const std::vector<DefSite> defs = computeDefSites(fn);
    for (il::BlockId header = 0; header < fn.blocks.size(); ++header) {
        std::vector<uint8_t> inLoop(fn.blocks.size(), 0);
        std::vector<il::BlockId> loopBlocks;
        for (il::BlockId pred : cfg.preds[header]) {
            if (!cfg.dominates(header, pred)) continue;
            for (il::BlockId b : naturalLoop(cfg, header, pred)) {
                if (b < inLoop.size() && !inLoop[b]) {
                    inLoop[b] = 1;
                    loopBlocks.push_back(b);
                }
            }
        }
        if (loopBlocks.empty()) continue;

        // The one block outside the loop that enters it, by a plain jump.
        il::BlockId preheader = il::kNoBlock;
        bool unique = true;
        for (il::BlockId pred : cfg.preds[header]) {
            if (inLoop[pred]) continue;
            if (preheader != il::kNoBlock) unique = false;
            preheader = pred;
        }
        if (!unique || preheader == il::kNoBlock || fn.blocks[preheader].instructions.empty()) continue;
        const il::Instruction& entry = fn.blocks[preheader].instructions.back();
        if (entry.op != il::Op::Jump || entry.target.block != header) continue;

        // operand -> the test already moved for it.
        std::vector<std::pair<il::ValueId, il::ValueId>> moved;
        std::vector<il::Instruction> hoisted;
        Rename rename;
        for (il::BlockId b : loopBlocks) {
            std::vector<il::Instruction>& insts = fn.blocks[b].instructions;
            for (size_t i = 0; i < insts.size();) {
                const il::Instruction& inst = insts[i];
                const il::ValueId v = isTruthTest(inst) ? inst.operands[0] : il::kNoValue;
                const bool outside = v != il::kNoValue && v < defs.size() &&
                                     (defs[v].block == il::kNoBlock || !inLoop[defs[v].block]);
                if (!outside) {
                    ++i;
                    continue;
                }
                il::ValueId first = il::kNoValue;
                for (const auto& [operand, test] : moved) {
                    if (operand == v) first = test;
                }
                if (first == il::kNoValue) {
                    moved.emplace_back(v, inst.result);
                    hoisted.push_back(inst);
                } else {
                    rename.emplace_back(inst.result, first);
                }
                insts.erase(insts.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        if (hoisted.empty()) continue;
        std::vector<il::Instruction>& pre = fn.blocks[preheader].instructions;
        pre.insert(pre.end() - 1, hoisted.begin(), hoisted.end());
        if (!rename.empty()) {
            for (il::Block& block : fn.blocks) {
                for (il::Instruction& inst : block.instructions) {
                    rewriteUses(inst, [&](il::ValueId id) { return renamed(rename, id); });
                }
            }
        }
        return true;
    }
    return false;
}

}  // namespace

bool hoistLoopInvariantTruths(il::Module& module) {
    bool changed = false;
    for (il::Function& fn : module.functions) {
        if (fn.blocks.empty()) continue;
        il::Function before = fn;
        bool any = false;
        // A test moved out of an inner loop is then the outer loop's to move.
        while (hoistOne(fn)) any = true;
        if (!any) continue;
        if (!dominanceHolds(fn)) {
            fn = std::move(before);
            continue;
        }
        changed = true;
    }
    return changed;
}

}  // namespace bronze::lower
