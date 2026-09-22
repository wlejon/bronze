#include "lower/loop_prop_hoist.h"

#include <algorithm>
#include <optional>
#include <vector>

#include "lower/guard_region_cfg.h"
#include "lower/loop_prop_analysis.h"

namespace bronze::lower {

namespace {

bool hoistInFunction(il::Function& fn, const std::vector<uint8_t>& isAccessorKey,
                     const il::Module& module,
                     std::vector<std::optional<SafeConstructorInfo>>& ctorCache,
                     const std::vector<FunctionEffects>& funcEffects,
                     uint32_t lengthKeyIndex) {
    if (fn.blocks.empty()) return false;

    bool fnChanged = false;
    bool keepGoing = true;
    while (keepGoing) {
        keepGoing = false;
        const Cfg cfg = buildCfg(fn);
        std::vector<DefSite> defs = computeDefSites(fn);

        // Find loop headers and their backedges
        for (il::BlockId header = 0; header < fn.blocks.size(); ++header) {
            std::vector<il::BlockId> backedges;
            for (il::BlockId pred : cfg.preds[header]) {
                if (cfg.dominates(header, pred)) {
                    backedges.push_back(pred);
                }
            }
            if (backedges.empty()) continue;

            // Form the natural loop as the union of loop blocks for all backedges to header
            std::vector<uint8_t> inLoop(fn.blocks.size(), 0);
            std::vector<il::BlockId> loopBlocks;
            for (il::BlockId tail : backedges) {
                const auto nat = naturalLoop(cfg, header, tail);
                for (il::BlockId b : nat) {
                    if (b < inLoop.size() && !inLoop[b]) {
                        inLoop[b] = 1;
                        loopBlocks.push_back(b);
                    }
                }
            }

            // Check if L has a unique preheader P:
            il::BlockId preheader = il::kNoBlock;
            bool uniquePreheader = true;
            for (il::BlockId pred : cfg.preds[header]) {
                if (!inLoop[pred]) {
                    if (preheader == il::kNoBlock) {
                        preheader = pred;
                    } else {
                        uniquePreheader = false;
                        break;
                    }
                }
            }

            if (!uniquePreheader || preheader == il::kNoBlock || preheader >= fn.blocks.size()) {
                continue;
            }

            // P terminates with a Jump to header
            il::Block& pBlock = fn.blocks[preheader];
            if (pBlock.instructions.empty()) continue;
            const auto& term = pBlock.instructions.back();
            if (term.op != il::Op::Jump || term.target.block != header) {
                continue;
            }

            // Compute blocks guaranteed to execute if the loop is entered (starting from header)
            std::vector<uint8_t> guaranteedToExecute(fn.blocks.size(), 0);
            il::BlockId curG = header;
            while (curG != il::kNoBlock && curG < fn.blocks.size() && inLoop[curG]) {
                guaranteedToExecute[curG] = 1;
                const auto& curBlk = fn.blocks[curG];
                if (curBlk.handler != il::kNoBlock) break;
                if (curBlk.instructions.empty()) break;
                const auto& curTerm = curBlk.instructions.back();
                if (curTerm.op == il::Op::Jump) {
                    il::BlockId next = curTerm.target.block;
                    if (next < cfg.preds.size() && cfg.preds[next].size() == 1) {
                        curG = next;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }

            // Find Loop-Invariant PropGets
            bool loopModified = false;
            for (il::BlockId b : loopBlocks) {
                il::Block& blk = fn.blocks[b];
                for (size_t i = 0; i < blk.instructions.size();) {
                    auto& inst = blk.instructions[i];
                    if (inst.op == il::Op::PropGet) {
                        if (inst.keyIndex < isAccessorKey.size() && isAccessorKey[inst.keyIndex]) {
                            ++i;
                            continue;
                        }
                        const il::ValueId recv = inst.operands[0];
                        const bool isInvariant =
                            recv < defs.size() &&
                            (defs[recv].block == il::kNoBlock || !inLoop[defs[recv].block]);
                        if (isInvariant) {
                            // Issue 1: Restrict hoisting to PropGet instructions in blocks
                            // that are guaranteed to execute if the loop is entered, or
                            // where the receiver is provably non-nullish.
                            if (!guaranteedToExecute[b] &&
                                !isProvablyNonNullish(recv, fn, defs, module, ctorCache)) {
                                ++i;
                                continue;
                            }

                            // Check if ANY instruction in the loop can mutate recv at inst.keyIndex
                            bool canMutate = false;
                            for (il::BlockId lb : loopBlocks) {
                                if (fn.blocks[lb].handler != il::kNoBlock) {
                                    canMutate = true;
                                    break;
                                }
                                for (const auto& loopInst : fn.blocks[lb].instructions) {
                                    if (canMutateProperty(recv, inst.keyIndex, loopInst, fn, defs, inLoop,
                                                          module, ctorCache, isAccessorKey, funcEffects,
                                                          lengthKeyIndex)) {
                                        canMutate = true;
                                        break;
                                    }
                                }
                                if (canMutate) break;
                            }

                            if (!canMutate) {
                                // Hoist into preheader immediately before terminating Jump
                                il::Instruction hoisted = inst;
                                const il::ValueId oldVal = inst.result;
                                const il::ValueId newVal = fn.valueCount++;
                                hoisted.result = newVal;

                                pBlock.instructions.insert(pBlock.instructions.end() - 1, hoisted);
                                if (defs.size() <= newVal) {
                                    defs.resize(newVal + 1);
                                }
                                defs[newVal] = DefSite{
                                    preheader, static_cast<uint32_t>(pBlock.instructions.size() - 2)};

                                // Replace all uses of oldVal in fn with newVal
                                for (il::Block& targetBlk : fn.blocks) {
                                    for (il::Instruction& userInst : targetBlk.instructions) {
                                        for (il::ValueId& op : userInst.operands) {
                                            if (op == oldVal) op = newVal;
                                        }
                                        for (il::ValueId& arg : userInst.target.args) {
                                            if (arg == oldVal) arg = newVal;
                                        }
                                        for (il::ValueId& arg : userInst.elseTarget.args) {
                                            if (arg == oldVal) arg = newVal;
                                        }
                                    }
                                }

                                // Erase original PropGet
                                blk.instructions.erase(blk.instructions.begin() + i);
                                loopModified = true;
                                fnChanged = true;
                                continue;
                            }
                        }
                    }
                    ++i;
                }
            }

            if (loopModified) {
                keepGoing = true;
                break;  // Rebuild CFG and def sites for next round
            }
        }
    }
    return fnChanged;
}

bool eliminateRedundantPropGets(il::Function& fn, const std::vector<uint8_t>& isAccessorKey,
                                const il::Module& module,
                                std::vector<std::optional<SafeConstructorInfo>>& ctorCache,
                                uint32_t lengthKeyIndex) {
    struct PropCell {
        il::ValueId recv;
        uint32_t keyIndex;
        il::ValueId val;
    };
    bool changed = false;
    std::vector<DefSite> defs = computeDefSites(fn);

    for (il::Block& blk : fn.blocks) {
        std::vector<PropCell> table;
        for (size_t i = 0; i < blk.instructions.size();) {
            auto& inst = blk.instructions[i];

            if (inst.op == il::Op::Construct) {
                const uint32_t calleeIdx = findCalleeIndex(inst, fn, defs, module);
                if (calleeIdx != UINT32_MAX && calleeIdx < ctorCache.size()) {
                    if (!ctorCache[calleeIdx].has_value()) {
                        ctorCache[calleeIdx] = analyzeConstructor(module.functions[calleeIdx]);
                    }
                    const auto& info = *ctorCache[calleeIdx];
                    if (info.isSafe) {
                        for (const auto& [kIdx, argIdx] : info.propArgMap) {
                            if (1 + argIdx < inst.operands.size()) {
                                table.push_back({inst.result, kIdx, inst.operands[1 + argIdx]});
                            }
                        }
                        ++i;
                        continue;
                    }
                }
                table.clear();
                ++i;
                continue;
            }

            if (inst.op == il::Op::PropSet) {
                if (inst.keyIndex < isAccessorKey.size() && isAccessorKey[inst.keyIndex]) {
                    table.clear();
                } else if (inst.keyIndex == lengthKeyIndex) {
                    table.clear();
                } else if (inst.operands.size() >= 2) {
                    const il::ValueId target = inst.operands[0];
                    const il::ValueId val = inst.operands[1];
                    const uint32_t key = inst.keyIndex;

                    if (!isKnownPlainObject(target, fn, defs, module, ctorCache)) {
                        table.clear();
                        ++i;
                        continue;
                    }

                    const bool isArrIdx = (key < module.keyConstants.size() &&
                                           isArrayIndexKey(module.keyConstants[key]));

                    if (isArrIdx) {
                        for (size_t t = 0; t < table.size();) {
                            if (table[t].keyIndex == lengthKeyIndex || table[t].keyIndex == key) {
                                table.erase(table.begin() + t);
                            } else {
                                ++t;
                            }
                        }
                    } else {
                        bool foundTarget = false;
                        for (size_t t = 0; t < table.size();) {
                            if (table[t].keyIndex == key) {
                                if (table[t].recv == target) {
                                    table[t].val = val;
                                    foundTarget = true;
                                    ++t;
                                } else {
                                    table.erase(table.begin() + t);
                                }
                            } else {
                                ++t;
                            }
                        }
                        if (!foundTarget) {
                            table.push_back({target, key, val});
                        }
                    }
                } else {
                    table.clear();
                }
                ++i;
                continue;
            }

            if (inst.op == il::Op::CreateObject || inst.op == il::Op::CreateArray) {
                ++i;
                continue;
            }

            if (inst.op == il::Op::MethodCall && !inst.operands.empty() &&
                isMathObject(inst.operands[0], fn, defs, module)) {
                ++i;
                continue;
            }

            if (isImpureForLoopPropHoist(inst)) {
                table.clear();
                ++i;
                continue;
            }

            if (inst.op == il::Op::PropGet && inst.result != il::kNoValue) {
                const il::ValueId recv = inst.operands[0];
                const uint32_t key = inst.keyIndex;
                const bool isArrIdx = (key < module.keyConstants.size() &&
                                       isArrayIndexKey(module.keyConstants[key]));
                if (!isArrIdx && key < isAccessorKey.size() && !isAccessorKey[key]) {
                    if (!isKnownPlainObject(recv, fn, defs, module, ctorCache)) {
                        table.clear();
                        ++i;
                        continue;
                    }
                    il::ValueId existing = il::kNoValue;
                    for (const auto& entry : table) {
                        if (entry.recv == recv && entry.keyIndex == key) {
                            existing = entry.val;
                            break;
                        }
                    }
                    if (existing != il::kNoValue) {
                        const il::ValueId oldVal = inst.result;
                        for (il::Block& targetBlk : fn.blocks) {
                            for (il::Instruction& userInst : targetBlk.instructions) {
                                for (il::ValueId& op : userInst.operands) {
                                    if (op == oldVal) op = existing;
                                }
                                for (il::ValueId& arg : userInst.target.args) {
                                    if (arg == oldVal) arg = existing;
                                }
                                for (il::ValueId& arg : userInst.elseTarget.args) {
                                    if (arg == oldVal) arg = existing;
                                }
                            }
                        }
                        blk.instructions.erase(blk.instructions.begin() + i);
                        defs = computeDefSites(fn);
                        changed = true;
                        continue;
                    }
                    table.push_back({recv, key, inst.result});
                }
            }
            ++i;
        }
    }

    // Prune dead fresh allocations and unused box operations
    std::vector<uint32_t> useCount(fn.valueCount, 0);
    for (const auto& b : fn.blocks) {
        for (const auto& inst : b.instructions) {
            for (il::ValueId op : inst.operands) {
                if (op < useCount.size()) useCount[op]++;
            }
            for (il::ValueId op : inst.target.args) {
                if (op < useCount.size()) useCount[op]++;
            }
            for (il::ValueId op : inst.elseTarget.args) {
                if (op < useCount.size()) useCount[op]++;
            }
        }
    }

    for (il::Block& blk : fn.blocks) {
        for (size_t i = 0; i < blk.instructions.size();) {
            auto& inst = blk.instructions[i];
            if (inst.result != il::kNoValue && useCount[inst.result] == 0) {
                bool canPrune = false;
                if (inst.op == il::Op::CreateObject || inst.op == il::Op::CreateArray) {
                    canPrune = true;
                } else if (inst.op == il::Op::Construct) {
                    const uint32_t calleeIdx = findCalleeIndex(inst, fn, defs, module);
                    if (calleeIdx != UINT32_MAX && calleeIdx < ctorCache.size()) {
                        if (!ctorCache[calleeIdx].has_value()) {
                            ctorCache[calleeIdx] = analyzeConstructor(module.functions[calleeIdx]);
                        }
                        if (ctorCache[calleeIdx]->isSafe) {
                            canPrune = true;
                        }
                    }
                }

                if (canPrune) {
                    for (il::ValueId op : inst.operands) {
                        if (op < useCount.size() && useCount[op] > 0) useCount[op]--;
                    }
                    blk.instructions.erase(blk.instructions.begin() + i);
                    changed = true;
                    continue;
                }
            }
            ++i;
        }
    }

    return changed;
}

}  // namespace

bool hoistLoopInvariantProps(il::Module& module) {
    std::vector<uint8_t> isAccessorKey(module.keyConstants.size(), 0);
    std::vector<uint8_t> isReflectionKey(module.keyConstants.size(), 0);
    uint32_t lengthKeyIndex = UINT32_MAX;

    for (size_t k = 0; k < module.keyConstants.size(); ++k) {
        const std::string& name = module.keyConstants[k];
        if (name == "Proxy" || name == "Reflect" || name == "defineProperty" ||
            name == "defineProperties" || name == "seal" || name == "freeze" ||
            name == "preventExtensions") {
            isReflectionKey[k] = 1;
        }
        if (name == "size" || name == "flags" || name == "source" || name == "buffer" ||
            name == "byteLength" || name == "byteOffset" || name == "__proto__" ||
            name == "lastIndex") {
            isAccessorKey[k] = 1;
        }
        if (name == "length") {
            lengthKeyIndex = static_cast<uint32_t>(k);
        }
    }

    for (const il::Function& fn : module.functions) {
        for (const il::Block& blk : fn.blocks) {
            for (const il::Instruction& inst : blk.instructions) {
                if (inst.op == il::Op::AccessorDef) {
                    if (inst.keyIndex < isAccessorKey.size()) {
                        isAccessorKey[inst.keyIndex] = 1;
                    }
                }
            }
        }
    }

    // Scoped reflection detection: check which functions touch dynamic reflection.
    std::vector<uint8_t> fnTouchesReflection(module.functions.size(), 0);
    for (size_t f = 0; f < module.functions.size(); ++f) {
        const auto& fn = module.functions[f];
        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::AccessorDefComputed ||
                    inst.op == il::Op::DefineOwnAttr) {
                    fnTouchesReflection[f] = 1;
                    break;
                }
                if (inst.keyIndex < isReflectionKey.size() && isReflectionKey[inst.keyIndex]) {
                    if (inst.op == il::Op::GlobalGet || inst.op == il::Op::PropGet ||
                        inst.op == il::Op::MethodCall || inst.op == il::Op::Call ||
                        inst.op == il::Op::DynamicCall || inst.op == il::Op::Construct) {
                        fnTouchesReflection[f] = 1;
                        break;
                    }
                }
            }
            if (fnTouchesReflection[f]) break;
        }
    }

    std::vector<std::optional<SafeConstructorInfo>> ctorCache(module.functions.size());
    const std::vector<FunctionEffects> funcEffects =
        computeModuleEffects(module, isAccessorKey, ctorCache, lengthKeyIndex);

    // Propagate reflection touch across direct callees
    bool refChanged = true;
    while (refChanged) {
        refChanged = false;
        for (size_t f = 0; f < module.functions.size(); ++f) {
            if (fnTouchesReflection[f]) continue;
            for (uint32_t callee : funcEffects[f].directCallees) {
                if (callee < fnTouchesReflection.size() && fnTouchesReflection[callee]) {
                    fnTouchesReflection[f] = 1;
                    refChanged = true;
                    break;
                }
            }
        }
    }

    bool anyChanged = false;
    bool keepGoing = true;
    while (keepGoing) {
        keepGoing = false;
        for (size_t f = 0; f < module.functions.size(); ++f) {
            if (fnTouchesReflection[f]) continue;
            il::Function& fn = module.functions[f];
            if (eliminateRedundantPropGets(fn, isAccessorKey, module, ctorCache, lengthKeyIndex)) {
                keepGoing = true;
                anyChanged = true;
            }
            if (hoistInFunction(fn, isAccessorKey, module, ctorCache, funcEffects, lengthKeyIndex)) {
                keepGoing = true;
                anyChanged = true;
            }
        }
    }
    return anyChanged;
}

}  // namespace bronze::lower
