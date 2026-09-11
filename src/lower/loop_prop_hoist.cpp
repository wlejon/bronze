#include "lower/loop_prop_hoist.h"

#include <algorithm>
#include <optional>
#include <vector>

#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

bool isImpureForLoopPropHoist(const il::Instruction& inst) {
    switch (inst.op) {
        // Property / Element / Private Stores and Mutators
        case il::Op::PropSet:
        case il::Op::ElemSet:
        case il::Op::ElemSetTyped:
        case il::Op::SuperSet:
        case il::Op::PrivateSet:
        case il::Op::PrivateAdd:
        case il::Op::MethodDef:
        case il::Op::MethodDefComputed:
        case il::Op::AccessorDef:
        case il::Op::AccessorDefComputed:
        case il::Op::DefineOwnAttr:
        case il::Op::PropDelete:
        case il::Op::ElemDelete:
        case il::Op::ClassExtend:
        // Environment Writes
        case il::Op::EnvSet:
        case il::Op::ModuleEnvSet:
        case il::Op::EnvInitTdz:
        case il::Op::ImmutableAssign:
        // Calls, Construction & Dynamic Import
        case il::Op::Call:
        case il::Op::DynamicCall:
        case il::Op::DynamicCallSpread:
        case il::Op::Construct:
        case il::Op::ConstructSpread:
        case il::Op::MethodCall:
        case il::Op::MethodCallSpread:
        case il::Op::SuperCall:
        case il::Op::SuperCallSpread:
        case il::Op::DynamicImport:
        // Control flow effects / Suspension
        case il::Op::Throw:
        case il::Op::AsyncAwait:
        case il::Op::AsyncIterOpen:
        case il::Op::AsyncIterNext:
        case il::Op::AsyncIterClose:
        case il::Op::IterDelegate:
            return true;
        default:
            return false;
    }
}

bool isArrayIndexKey(const std::string& key) {
    if (key.empty() || key.size() > 10) return false;
    if (key.size() > 1 && key[0] == '0') return false;
    uint64_t n = 0;
    for (const char c : key) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + (c - '0');
        if (n >= 0xFFFFFFFFULL) return false;
    }
    return true;
}

struct SafeConstructorInfo {
    bool isSafe = false;
    std::vector<std::pair<uint32_t, uint32_t>> propArgMap;
};

SafeConstructorInfo analyzeConstructor(const il::Function& calleeFn) {
    SafeConstructorInfo info;
    for (const auto& blk : calleeFn.blocks) {
        if (blk.handler != il::kNoBlock) return info;
    }
    if (!calleeFn.needsThis) return info;
    const il::ValueId thisVal = calleeFn.needsEnv ? 1u : 0u;
    const size_t firstSource = calleeFn.firstSourceParam();

    for (const auto& blk : calleeFn.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == il::Op::PropSet) {
                if (inst.operands.empty() || inst.operands[0] != thisVal) {
                    return info;
                }
                if (inst.operands.size() > 1) {
                    const il::ValueId valId = inst.operands[1];
                    if (valId >= firstSource && valId < calleeFn.params.size()) {
                        uint32_t argIdx = static_cast<uint32_t>(valId - firstSource);
                        bool replaced = false;
                        for (auto& entry : info.propArgMap) {
                            if (entry.first == inst.keyIndex) {
                                entry.second = argIdx;
                                replaced = true;
                                break;
                            }
                        }
                        if (!replaced) {
                            info.propArgMap.push_back({inst.keyIndex, argIdx});
                        }
                    }
                }
                continue;
            }

            if (isImpureForLoopPropHoist(inst)) {
                return info;
            }
        }
    }
    info.isSafe = true;
    return info;
}

uint32_t findCalleeIndex(const il::Instruction& inst, const il::Function& fn,
                         const std::vector<DefSite>& defs, const il::Module& module) {
    if (inst.op == il::Op::Call) {
        if (inst.calleeIndex < module.functions.size()) {
            return inst.calleeIndex;
        }
        return UINT32_MAX;
    }
    if (inst.directTarget != il::Instruction::kNoDirectTarget &&
        inst.directTarget < module.functions.size()) {
        return inst.directTarget;
    }
    if (!inst.operands.empty()) {
        const il::ValueId calleeVal = inst.operands[0];
        if (calleeVal < defs.size()) {
            const auto& def = defs[calleeVal];
            if (def.block != il::kNoBlock && def.block < fn.blocks.size()) {
                const auto& blk = fn.blocks[def.block];
                if (def.index < blk.instructions.size()) {
                    const auto& defInst = blk.instructions[def.index];
                    if (defInst.op == il::Op::FunctionRef &&
                        defInst.calleeIndex < module.functions.size()) {
                        return defInst.calleeIndex;
                    }
                }
            }
        }
    }
    return UINT32_MAX;
}

bool isFreshLocalObject(il::ValueId val, const il::Function& fn, const std::vector<DefSite>& defs,
                        const std::vector<uint8_t>& inLoop) {
    if (val >= defs.size()) return false;
    const auto& def = defs[val];
    if (def.block == il::kNoBlock || def.block >= fn.blocks.size()) return false;
    if (!inLoop.empty() && (def.block >= inLoop.size() || !inLoop[def.block])) return false;
    const auto& b = fn.blocks[def.block];
    if (def.index >= b.instructions.size()) return false;
    const auto& inst = b.instructions[def.index];
    return inst.op == il::Op::Construct ||
           inst.op == il::Op::ConstructSpread ||
           inst.op == il::Op::CreateObject ||
           inst.op == il::Op::CreateArray;
}

bool isKnownPlainObject(il::ValueId val, const il::Function& fn, const std::vector<DefSite>& defs,
                        const il::Module& module,
                        std::vector<std::optional<SafeConstructorInfo>>& ctorCache) {
    if (val >= defs.size()) return false;
    const auto& def = defs[val];
    if (def.block == il::kNoBlock || def.block >= fn.blocks.size()) return false;
    const auto& blk = fn.blocks[def.block];
    if (def.index >= blk.instructions.size()) return false;
    const auto& inst = blk.instructions[def.index];
    if (inst.op == il::Op::CreateObject) return true;
    if (inst.op == il::Op::Construct) {
        const uint32_t calleeIdx = findCalleeIndex(inst, fn, defs, module);
        if (calleeIdx != UINT32_MAX && calleeIdx < ctorCache.size()) {
            if (!ctorCache[calleeIdx].has_value()) {
                ctorCache[calleeIdx] = analyzeConstructor(module.functions[calleeIdx]);
            }
            if (ctorCache[calleeIdx]->isSafe) return true;
        }
    }
    return false;
}

bool isMathObject(il::ValueId val, const il::Function& fn, const std::vector<DefSite>& defs,
                  const il::Module& module) {
    if (val >= defs.size()) return false;
    const auto& def = defs[val];
    if (def.block == il::kNoBlock || def.block >= fn.blocks.size()) return false;
    const auto& blk = fn.blocks[def.block];
    if (def.index >= blk.instructions.size()) return false;
    const auto& inst = blk.instructions[def.index];
    return inst.op == il::Op::GlobalGet &&
           inst.keyIndex < module.keyConstants.size() &&
           module.keyConstants[inst.keyIndex] == "Math";
}

bool isArrayMutatingMethod(const std::string& name) {
    return name == "push" || name == "pop" || name == "shift" ||
           name == "unshift" || name == "splice" || name == "reverse" ||
           name == "sort" || name == "fill" || name == "copyWithin";
}

bool isArrayReadOnlyMethod(const std::string& name) {
    return name == "indexOf" || name == "lastIndexOf" || name == "includes" ||
           name == "slice" || name == "join" || name == "concat" ||
           name == "at" || name == "toString";
}

struct FunctionEffects {
    bool hasUnresolvedCalls = false;
    bool mutatesLength = false;
    std::vector<uint8_t> writtenKeys;
    std::vector<uint32_t> directCallees;
};

std::vector<FunctionEffects> computeModuleEffects(
    const il::Module& module,
    const std::vector<uint8_t>& isAccessorKey,
    std::vector<std::optional<SafeConstructorInfo>>& ctorCache,
    uint32_t lengthKeyIndex) {
    std::vector<FunctionEffects> effects(module.functions.size());

    for (size_t f = 0; f < module.functions.size(); ++f) {
        const auto& fn = module.functions[f];
        effects[f].writtenKeys.assign(module.keyConstants.size(), 0);
        std::vector<DefSite> defs = computeDefSites(fn);

        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::PropSet) {
                    if (inst.keyIndex < isAccessorKey.size() && isAccessorKey[inst.keyIndex]) {
                        effects[f].hasUnresolvedCalls = true;
                    } else if (inst.keyIndex < effects[f].writtenKeys.size()) {
                        effects[f].writtenKeys[inst.keyIndex] = 1;
                    }
                    if (inst.keyIndex == lengthKeyIndex ||
                        (inst.keyIndex < module.keyConstants.size() &&
                         isArrayIndexKey(module.keyConstants[inst.keyIndex]))) {
                        effects[f].mutatesLength = true;
                    }
                } else if (inst.op == il::Op::ElemSet || inst.op == il::Op::ElemSetTyped) {
                    effects[f].mutatesLength = true;
                } else if (inst.op == il::Op::MethodCall) {
                    if (!inst.operands.empty() && isMathObject(inst.operands[0], fn, defs, module)) {
                        // Pure Math method
                    } else if (inst.directTarget != il::Instruction::kNoDirectTarget &&
                               inst.directTarget < module.functions.size()) {
                        effects[f].directCallees.push_back(inst.directTarget);
                    } else {
                        const std::string& name = (inst.keyIndex < module.keyConstants.size())
                                                      ? module.keyConstants[inst.keyIndex]
                                                      : "";
                        if (isArrayMutatingMethod(name)) {
                            effects[f].mutatesLength = true;
                        } else if (!isArrayReadOnlyMethod(name)) {
                            effects[f].hasUnresolvedCalls = true;
                        }
                    }
                } else if (inst.op == il::Op::Call) {
                    const uint32_t callee = findCalleeIndex(inst, fn, defs, module);
                    if (callee != UINT32_MAX && callee < module.functions.size()) {
                        effects[f].directCallees.push_back(callee);
                    } else {
                        effects[f].hasUnresolvedCalls = true;
                    }
                } else if (inst.op == il::Op::Construct) {
                    const uint32_t callee = findCalleeIndex(inst, fn, defs, module);
                    if (callee != UINT32_MAX && callee < ctorCache.size()) {
                        if (!ctorCache[callee].has_value()) {
                            ctorCache[callee] = analyzeConstructor(module.functions[callee]);
                        }
                        if (!ctorCache[callee]->isSafe) {
                            effects[f].hasUnresolvedCalls = true;
                        }
                    } else {
                        effects[f].hasUnresolvedCalls = true;
                    }
                } else if (inst.op == il::Op::CreateObject || inst.op == il::Op::CreateArray) {
                    // Pure
                } else if (isImpureForLoopPropHoist(inst)) {
                    effects[f].hasUnresolvedCalls = true;
                }
            }
        }
    }

    // Fixed-point propagation
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t f = 0; f < effects.size(); ++f) {
            for (uint32_t callee : effects[f].directCallees) {
                if (effects[callee].hasUnresolvedCalls && !effects[f].hasUnresolvedCalls) {
                    effects[f].hasUnresolvedCalls = true;
                    changed = true;
                }
                if (effects[callee].mutatesLength && !effects[f].mutatesLength) {
                    effects[f].mutatesLength = true;
                    changed = true;
                }
                for (size_t k = 0; k < effects[callee].writtenKeys.size(); ++k) {
                    if (effects[callee].writtenKeys[k] && !effects[f].writtenKeys[k]) {
                        effects[f].writtenKeys[k] = 1;
                        changed = true;
                    }
                }
            }
        }
    }

    return effects;
}

bool canMutateProperty(il::ValueId recv, uint32_t keyIndex,
                       const il::Instruction& inst, const il::Function& fn,
                       const std::vector<DefSite>& defs,
                       const std::vector<uint8_t>& inLoop,
                       const il::Module& module,
                       std::vector<std::optional<SafeConstructorInfo>>& ctorCache,
                       const std::vector<uint8_t>& isAccessorKey,
                       const std::vector<FunctionEffects>& funcEffects,
                       uint32_t lengthKeyIndex) {
    switch (inst.op) {
        case il::Op::PropSet: {
            if (inst.operands.empty()) return true;
            if (inst.keyIndex < isAccessorKey.size() && isAccessorKey[inst.keyIndex]) {
                return true;
            }
            if (keyIndex == lengthKeyIndex &&
                inst.keyIndex < module.keyConstants.size() &&
                isArrayIndexKey(module.keyConstants[inst.keyIndex])) {
                const il::ValueId target = inst.operands[0];
                if (isFreshLocalObject(target, fn, defs, inLoop)) {
                    return false;
                }
                return true;
            }
            if (inst.keyIndex != keyIndex) {
                return false;
            }
            const il::ValueId target = inst.operands[0];
            if (isFreshLocalObject(target, fn, defs, inLoop)) {
                return false;
            }
            return true;
        }

        case il::Op::ElemSet:
        case il::Op::ElemSetTyped: {
            if (inst.operands.empty()) return true;
            if (keyIndex != lengthKeyIndex) {
                return false;
            }
            const il::ValueId target = inst.operands[0];
            if (isFreshLocalObject(target, fn, defs, inLoop)) {
                return false;
            }
            return true;
        }

        case il::Op::Construct: {
            const uint32_t calleeIdx = findCalleeIndex(inst, fn, defs, module);
            if (calleeIdx == UINT32_MAX || calleeIdx >= ctorCache.size()) return true;
            if (!ctorCache[calleeIdx].has_value()) {
                ctorCache[calleeIdx] = analyzeConstructor(module.functions[calleeIdx]);
            }
            return !ctorCache[calleeIdx]->isSafe;
        }

        case il::Op::CreateObject:
        case il::Op::CreateArray:
            return false;

        case il::Op::MethodCall: {
            if (inst.operands.empty()) return true;
            const il::ValueId target = inst.operands[0];
            if (isMathObject(target, fn, defs, module)) {
                return false;
            }
            if (inst.directTarget != il::Instruction::kNoDirectTarget &&
                inst.directTarget < funcEffects.size()) {
                const auto& eff = funcEffects[inst.directTarget];
                if (!eff.hasUnresolvedCalls) {
                    if (keyIndex == lengthKeyIndex && eff.mutatesLength) {
                        if (target == recv) return true;
                    }
                    if (keyIndex < eff.writtenKeys.size() && eff.writtenKeys[keyIndex]) {
                        for (il::ValueId op : inst.operands) {
                            if (op == recv) return true;
                        }
                        return false;
                    }
                    return false;
                }
            }
            const uint32_t methodKey = inst.keyIndex;
            const std::string& methodName =
                (methodKey < module.keyConstants.size()) ? module.keyConstants[methodKey] : "";

            if (isArrayMutatingMethod(methodName)) {
                if (keyIndex != lengthKeyIndex) {
                    return false;
                }
                if (target == recv) return true;
                if (isFreshLocalObject(target, fn, defs, inLoop)) return false;
                if (isFreshLocalObject(target, fn, defs, /*inLoop=*/{})) {
                    if (!isFreshLocalObject(recv, fn, defs, /*inLoop=*/{}) || target != recv) {
                        return false;
                    }
                }
                return true;
            }
            if (isArrayReadOnlyMethod(methodName)) {
                return false;
            }
            return true;
        }

        case il::Op::Call: {
            const uint32_t callee = findCalleeIndex(inst, fn, defs, module);
            if (callee != UINT32_MAX && callee < funcEffects.size()) {
                const auto& eff = funcEffects[callee];
                if (!eff.hasUnresolvedCalls) {
                    if (keyIndex == lengthKeyIndex && eff.mutatesLength) {
                        return true;
                    }
                    if (keyIndex < eff.writtenKeys.size() && eff.writtenKeys[keyIndex]) {
                        for (il::ValueId op : inst.operands) {
                            if (op == recv) return true;
                        }
                        return false;
                    }
                    return false;
                }
            }
            return true;
        }

        case il::Op::SuperSet:
        case il::Op::PrivateSet:
        case il::Op::PrivateAdd:
        case il::Op::MethodDef:
        case il::Op::MethodDefComputed:
        case il::Op::AccessorDef:
        case il::Op::AccessorDefComputed:
        case il::Op::DefineOwnAttr:
        case il::Op::PropDelete:
        case il::Op::ElemDelete:
        case il::Op::ClassExtend:
        case il::Op::EnvSet:
        case il::Op::ModuleEnvSet:
        case il::Op::EnvInitTdz:
        case il::Op::ImmutableAssign:
        case il::Op::DynamicCall:
        case il::Op::DynamicCallSpread:
        case il::Op::ConstructSpread:
        case il::Op::MethodCallSpread:
        case il::Op::SuperCall:
        case il::Op::SuperCallSpread:
        case il::Op::DynamicImport:
        case il::Op::Throw:
        case il::Op::AsyncAwait:
        case il::Op::AsyncIterOpen:
        case il::Op::AsyncIterNext:
        case il::Op::AsyncIterClose:
        case il::Op::IterDelegate:
            return true;

        default:
            return false;
    }
}

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
    bool moduleUsesProxy = false;
    bool moduleHasDynamicAccessors = false;
    std::vector<uint8_t> isAccessorKey(module.keyConstants.size(), 0);
    uint32_t lengthKeyIndex = UINT32_MAX;

    for (size_t k = 0; k < module.keyConstants.size(); ++k) {
        const std::string& name = module.keyConstants[k];
        if (name == "Proxy") {
            moduleUsesProxy = true;
        }
        if (name == "Reflect" || name == "defineProperty" || name == "defineProperties" ||
            name == "seal" || name == "freeze" || name == "preventExtensions") {
            moduleHasDynamicAccessors = true;
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
                } else if (inst.op == il::Op::AccessorDefComputed ||
                           inst.op == il::Op::DefineOwnAttr) {
                    moduleHasDynamicAccessors = true;
                }
            }
        }
    }

    if (moduleUsesProxy || moduleHasDynamicAccessors) {
        return false;
    }

    std::vector<std::optional<SafeConstructorInfo>> ctorCache(module.functions.size());
    const std::vector<FunctionEffects> funcEffects =
        computeModuleEffects(module, isAccessorKey, ctorCache, lengthKeyIndex);

    bool anyChanged = false;
    bool keepGoing = true;
    while (keepGoing) {
        keepGoing = false;
        for (il::Function& fn : module.functions) {
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
