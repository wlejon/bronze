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

bool canMutateRecvOrPrototypes(const il::Instruction& inst, const il::Function& fn,
                               const std::vector<DefSite>& defs, const std::vector<uint8_t>& inLoop,
                               const il::Module& module,
                               std::vector<std::optional<SafeConstructorInfo>>& ctorCache) {
    switch (inst.op) {
        case il::Op::PropSet: {
            if (inst.operands.empty()) return true;
            const il::ValueId target = inst.operands[0];
            return !isFreshLocalObject(target, fn, defs, inLoop);
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
        case il::Op::ElemSet:
        case il::Op::ElemSetTyped: {
            if (inst.operands.empty()) return true;
            const il::ValueId target = inst.operands[0];
            return !isFreshLocalObject(target, fn, defs, inLoop);
        }
        case il::Op::Call:
        case il::Op::DynamicCall:
        case il::Op::DynamicCallSpread:
        case il::Op::ConstructSpread:
        case il::Op::MethodCall:
        case il::Op::MethodCallSpread:
        case il::Op::SuperCall:
        case il::Op::SuperCallSpread:
        case il::Op::DynamicImport:
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
                     std::vector<std::optional<SafeConstructorInfo>>& ctorCache) {
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
                            // Check if ANY instruction in the loop can mutate recv or its prototypes
                            bool canMutate = false;
                            for (il::BlockId lb : loopBlocks) {
                                if (fn.blocks[lb].handler != il::kNoBlock) {
                                    canMutate = true;
                                    break;
                                }
                                for (const auto& loopInst : fn.blocks[lb].instructions) {
                                    if (canMutateRecvOrPrototypes(loopInst, fn, defs, inLoop, module, ctorCache)) {
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
                                std::vector<std::optional<SafeConstructorInfo>>& ctorCache) {
    struct Avail {
        il::ValueId recv;
        uint32_t keyIndex;
        il::ValueId val;
        Avail(il::ValueId r, uint32_t k, il::ValueId v) : recv(r), keyIndex(k), val(v) {}
    };
    bool changed = false;
    std::vector<DefSite> defs = computeDefSites(fn);

    for (il::Block& blk : fn.blocks) {
        std::vector<Avail> avail;
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
                                avail.push_back(Avail(inst.result, kIdx, inst.operands[1 + argIdx]));
                            }
                        }
                        ++i;
                        continue;
                    }
                }
                avail.clear();
                ++i;
                continue;
            }

            if (inst.op == il::Op::PropSet) {
                if (!inst.operands.empty()) {
                    const il::ValueId target = inst.operands[0];
                    for (size_t a = 0; a < avail.size();) {
                        if (avail[a].recv == target) {
                            avail.erase(avail.begin() + a);
                        } else {
                            ++a;
                        }
                    }
                    if (isFreshLocalObject(target, fn, defs, /*inLoop=*/{})) {
                        ++i;
                        continue;
                    }
                }
                avail.clear();
                ++i;
                continue;
            }

            if (inst.op == il::Op::CreateObject || inst.op == il::Op::CreateArray) {
                ++i;
                continue;
            }

            if (isImpureForLoopPropHoist(inst)) {
                avail.clear();
                ++i;
                continue;
            }

            if (inst.op == il::Op::PropGet && inst.result != il::kNoValue) {
                const il::ValueId recv = inst.operands[0];
                const uint32_t key = inst.keyIndex;
                if (key < isAccessorKey.size() && !isAccessorKey[key]) {
                    il::ValueId existing = il::kNoValue;
                    for (const auto& a : avail) {
                        if (a.recv == recv && a.keyIndex == key) {
                            existing = a.val;
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
                    avail.push_back(Avail(recv, key, inst.result));
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

    for (size_t k = 0; k < module.keyConstants.size(); ++k) {
        const std::string& name = module.keyConstants[k];
        if (name == "Proxy") {
            moduleUsesProxy = true;
        }
        if (name == "Reflect" || name == "defineProperty" || name == "defineProperties") {
            moduleHasDynamicAccessors = true;
        }
        if (name == "size" || name == "flags" || name == "source" || name == "buffer" ||
            name == "byteLength" || name == "byteOffset" || name == "__proto__") {
            isAccessorKey[k] = 1;
        }
    }

    for (const il::Function& fn : module.functions) {
        for (const il::Block& blk : fn.blocks) {
            for (const il::Instruction& inst : blk.instructions) {
                if (inst.op == il::Op::AccessorDef) {
                    if (inst.keyIndex < isAccessorKey.size()) {
                        isAccessorKey[inst.keyIndex] = 1;
                    }
                } else if (inst.op == il::Op::AccessorDefComputed) {
                    moduleHasDynamicAccessors = true;
                }
            }
        }
    }

    if (moduleUsesProxy || moduleHasDynamicAccessors) {
        return false;
    }

    bool anyChanged = false;
    std::vector<std::optional<SafeConstructorInfo>> ctorCache(module.functions.size());
    bool keepGoing = true;
    while (keepGoing) {
        keepGoing = false;
        for (il::Function& fn : module.functions) {
            if (eliminateRedundantPropGets(fn, isAccessorKey, module, ctorCache)) {
                keepGoing = true;
                anyChanged = true;
            }
            if (hoistInFunction(fn, isAccessorKey, module, ctorCache)) {
                keepGoing = true;
                anyChanged = true;
            }
        }
    }
    return anyChanged;
}

}  // namespace bronze::lower
