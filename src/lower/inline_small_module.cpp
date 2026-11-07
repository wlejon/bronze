// The module facts the small-function inliner reads (inline_small.h): whether
// any prototype can be patched, which method keys are ever written, the closed
// set of functions stored under each key, and which direct method calls are
// sealed. Split from inline_small.cpp.

#include "lower/inline_small.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lower/loop_prop_analysis.h"

namespace bronze::lower {

namespace {

constexpr uint32_t kNoKey = UINT32_MAX;

std::vector<uint8_t> computeDeadFunctions(const il::Module& module) {
    const size_t numKeys = module.keyConstants.size();
    const size_t numFns = module.functions.size();
    std::vector<uint8_t> isDeadFn(numFns, 0);
    if (numFns == 0) return isDeadFn;

    std::vector<uint8_t> keyUsedOutsideMethodDef(numKeys, 0);
    std::vector<uint8_t> fnReferencedDirectly(numFns, 0);
    fnReferencedDirectly[0] = 1;

    for (const auto& fn : module.functions) {
        if (fn.isExported || fn.isEntryPoint) {
            for (size_t i = 0; i < numFns; ++i) {
                if (&module.functions[i] == &fn) fnReferencedDirectly[i] = 1;
            }
        }
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op != il::Op::MethodDef && inst.op != il::Op::EnvGetTdz &&
                    inst.keyIndex < numKeys) {
                    keyUsedOutsideMethodDef[inst.keyIndex] = 1;
                }
                if (inst.op == il::Op::Call && inst.calleeIndex < numFns) {
                    fnReferencedDirectly[inst.calleeIndex] = 1;
                }
                if (inst.op == il::Op::FunctionRef && inst.calleeIndex < numFns) {
                    fnReferencedDirectly[inst.calleeIndex] = 1;
                }
                if (inst.directTarget != il::Instruction::kNoDirectTarget &&
                    inst.directTarget < numFns) {
                    fnReferencedDirectly[inst.directTarget] = 1;
                }
            }
        }
    }

    for (const auto& fn : module.functions) {
        std::vector<int32_t> createFuncTarget(fn.valueCount, -1);
        std::vector<uint32_t> useCount(fn.valueCount, 0);
        std::vector<uint32_t> deadMethodDefUseCount(fn.valueCount, 0);

        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == il::Op::CreateFunction && inst.result < fn.valueCount &&
                    inst.calleeIndex < numFns) {
                    createFuncTarget[inst.result] = static_cast<int32_t>(inst.calleeIndex);
                }
                for (size_t opIdx = 0; opIdx < inst.operands.size(); ++opIdx) {
                    const il::ValueId op = inst.operands[opIdx];
                    if (op >= fn.valueCount) continue;
                    ++useCount[op];
                    if (inst.op == il::Op::MethodDef && opIdx == 1 && inst.keyIndex < numKeys &&
                        !keyUsedOutsideMethodDef[inst.keyIndex]) {
                        ++deadMethodDefUseCount[op];
                    }
                }
            }
        }

        for (il::ValueId v = 0; v < fn.valueCount; ++v) {
            const int32_t targetFn = createFuncTarget[v];
            if (targetFn < 0) continue;
            const auto targetIdx = static_cast<size_t>(targetFn);
            if (!fnReferencedDirectly[targetIdx] && useCount[v] > 0 &&
                useCount[v] == deadMethodDefUseCount[v]) {
                isDeadFn[targetIdx] = 1;
            }
        }
    }

    return isDeadFn;
}

}  // namespace

bool isModulePrototypePatchFree(const il::Module& module) {
    uint32_t prototypeKeyIdx = kNoKey;
    uint32_t protoKeyIdx = kNoKey;
    uint32_t definePropertyKeyIdx = kNoKey;
    uint32_t definePropertiesKeyIdx = kNoKey;
    uint32_t symbolKeyIdx = kNoKey;

    for (uint32_t i = 0; i < module.keyConstants.size(); ++i) {
        const std::string& k = module.keyConstants[i];
        if (k == "setPrototypeOf" || k == "Proxy" || k == "Reflect") {
            return false;
        }
        if (k == "prototype") prototypeKeyIdx = i;
        else if (k == "__proto__") protoKeyIdx = i;
        else if (k == "defineProperty") definePropertyKeyIdx = i;
        else if (k == "defineProperties") definePropertiesKeyIdx = i;
        else if (k == "Symbol") symbolKeyIdx = i;
    }

    for (const auto& fn : module.functions) {
        std::vector<uint8_t> isProtoVal(fn.valueCount, 0);
        std::vector<uint8_t> isCreateObjVal(fn.valueCount, 0);
        std::vector<uint8_t> isConstStrVal(fn.valueCount, 0);
        std::vector<uint8_t> isSymbolGlobal(fn.valueCount, 0);
        std::vector<uint8_t> isSymbolKeyVal(fn.valueCount, 0);

        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.result == il::kNoValue || inst.result >= fn.valueCount) continue;
                if (inst.op == il::Op::CreateObject) {
                    isCreateObjVal[inst.result] = 1;
                } else if (inst.op == il::Op::Box && inst.boxType == il::Type::Str &&
                           inst.operands.empty() && inst.keyIndex < module.keyConstants.size()) {
                    isConstStrVal[inst.result] = 1;
                } else if (inst.op == il::Op::GlobalGet && symbolKeyIdx != kNoKey &&
                           inst.keyIndex == symbolKeyIdx) {
                    isSymbolGlobal[inst.result] = 1;
                } else if (inst.op == il::Op::PropGet) {
                    if (prototypeKeyIdx != kNoKey && inst.keyIndex == prototypeKeyIdx) {
                        isProtoVal[inst.result] = 1;
                    }
                    if (!inst.operands.empty() && inst.operands[0] < fn.valueCount &&
                        isSymbolGlobal[inst.operands[0]]) {
                        isSymbolKeyVal[inst.result] = 1;
                    }
                }
            }
        }

        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == il::Op::AccessorDefComputed) {
                    return false;
                }
                if (inst.op == il::Op::MethodDefComputed) {
                    if (inst.operands.size() < 2 || inst.operands[1] >= fn.valueCount ||
                        !isSymbolKeyVal[inst.operands[1]]) {
                        return false;
                    }
                }
                if (protoKeyIdx != kNoKey && inst.keyIndex == protoKeyIdx) {
                    if (inst.op == il::Op::PropSet) {
                        if (inst.operands.empty() || inst.operands[0] >= fn.valueCount ||
                            !isCreateObjVal[inst.operands[0]]) {
                            return false;
                        }
                    } else if (inst.op == il::Op::PropGet || inst.op == il::Op::DefineOwnAttr ||
                               inst.op == il::Op::PropDelete || inst.op == il::Op::AccessorDef ||
                               inst.op == il::Op::MethodDef) {
                        return false;
                    }
                }
                if (inst.op == il::Op::MethodCall) {
                    if (definePropertiesKeyIdx != kNoKey &&
                        inst.keyIndex == definePropertiesKeyIdx) {
                        return false;
                    }
                    if (definePropertyKeyIdx != kNoKey &&
                        inst.keyIndex == definePropertyKeyIdx) {
                        if (inst.operands.size() < 3 || inst.operands[2] >= fn.valueCount ||
                            !isConstStrVal[inst.operands[2]]) {
                            return false;
                        }
                    }
                }
                for (size_t opIdx = 0; opIdx < inst.operands.size(); ++opIdx) {
                    const il::ValueId op = inst.operands[opIdx];
                    if (op >= fn.valueCount || !isProtoVal[op]) continue;
                    const bool allowedOnReceiver =
                        opIdx == 0 &&
                        (inst.op == il::Op::MethodDef || inst.op == il::Op::AccessorDef ||
                         inst.op == il::Op::MethodDefComputed || inst.op == il::Op::PropSet ||
                         inst.op == il::Op::PropDelete || inst.op == il::Op::PropGet ||
                         inst.op == il::Op::SuperGet);
                    if (!allowedOnReceiver) return false;
                }
            }
        }
    }

    return true;
}

std::vector<uint8_t> computeMutatedMethodKeyMask(const il::Module& module) {
    const size_t numKeys = module.keyConstants.size();
    std::vector<uint8_t> mutated(numKeys, 0);
    if (numKeys == 0) return mutated;

    uint32_t definePropertyKeyIdx = kNoKey;
    for (uint32_t i = 0; i < numKeys; ++i) {
        if (module.keyConstants[i] == "defineProperty") {
            definePropertyKeyIdx = i;
            break;
        }
    }

    const std::vector<uint8_t> isDeadFn = computeDeadFunctions(module);

    for (size_t fnIdx = 0; fnIdx < module.functions.size(); ++fnIdx) {
        if (isDeadFn[fnIdx]) continue;
        const auto& fn = module.functions[fnIdx];
        std::vector<uint32_t> constStrKeyIdx(fn.valueCount, kNoKey);
        std::vector<uint8_t> isCreateObjVal(fn.valueCount, 0);

        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.result != il::kNoValue && inst.result < fn.valueCount) {
                    if (inst.op == il::Op::CreateObject) {
                        isCreateObjVal[inst.result] = 1;
                    } else if (inst.op == il::Op::Box && inst.boxType == il::Type::Str &&
                               inst.operands.empty() && inst.keyIndex < numKeys) {
                        constStrKeyIdx[inst.result] = inst.keyIndex;
                    }
                }
            }
        }

        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == il::Op::PropSet || inst.op == il::Op::DefineOwnAttr ||
                    inst.op == il::Op::PropDelete || inst.op == il::Op::AccessorDef) {
                    if ((inst.op == il::Op::PropSet || inst.op == il::Op::DefineOwnAttr) &&
                        !inst.operands.empty() && inst.operands[0] < fn.valueCount &&
                        isCreateObjVal[inst.operands[0]]) {
                        continue;
                    }
                    if (inst.keyIndex < numKeys) {
                        mutated[inst.keyIndex] = 1;
                    }
                } else if (inst.op == il::Op::MethodCall &&
                           definePropertyKeyIdx != kNoKey &&
                           inst.keyIndex == definePropertyKeyIdx &&
                           inst.operands.size() >= 3 &&
                           inst.operands[2] < fn.valueCount) {
                    const uint32_t kIdx = constStrKeyIdx[inst.operands[2]];
                    if (kIdx < numKeys) {
                        mutated[kIdx] = 1;
                    }
                }
            }
        }
    }

    return mutated;
}

std::vector<std::vector<uint32_t>> computeClosedKeyTargets(const il::Module& module) {
    const size_t numKeys = module.keyConstants.size();
    const size_t numFns = module.functions.size();
    std::vector<std::vector<uint32_t>> targets(numKeys);
    if (numKeys == 0 || numFns == 0 || !isModulePrototypePatchFree(module)) {
        return targets;
    }

    std::vector<uint8_t> valid(numKeys, 1);
    for (uint32_t k = 0; k < numKeys; ++k) {
        const std::string& name = module.keyConstants[k];
        if (name == "call" || name == "apply" || name == "bind" ||
            name == "toString" || name == "valueOf" || name == "assign" ||
            name == "defineProperty" || name == "defineProperties" ||
            name == "setPrototypeOf" || isArrayMutatingMethod(name) ||
            isArrayReadOnlyMethod(name)) {
            valid[k] = 0;
        }
    }

    struct ForwardedSetter {
        uint32_t setterFnIdx;
        uint32_t paramIdx;
        uint32_t targetKey;
    };
    std::vector<ForwardedSetter> forwarded;
    std::unordered_map<uint32_t, uint32_t> methodFnToKey;
    std::vector<uint8_t> keyMutatedByPropSet(numKeys, 0);

    std::vector<std::vector<int32_t>> allFuncValCallee(numFns);
    for (size_t fnIdx = 0; fnIdx < numFns; ++fnIdx) {
        const auto& fn = module.functions[fnIdx];
        const il::ValueId thisParamId = fn.needsEnv ? 1u : 0u;
        auto& funcValCallee = allFuncValCallee[fnIdx];
        funcValCallee.assign(fn.valueCount, -1);
        std::vector<uint8_t> isNullishConst(fn.valueCount, 0);
        std::vector<uint8_t> isCreateObjVal(fn.valueCount, 0);

        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.result != il::kNoValue && inst.result < fn.valueCount) {
                    if ((inst.op == il::Op::CreateFunction || inst.op == il::Op::FunctionRef) &&
                        inst.calleeIndex < numFns) {
                        funcValCallee[inst.result] = static_cast<int32_t>(inst.calleeIndex);
                    } else if (inst.op == il::Op::ConstUndefined || inst.op == il::Op::ConstNull) {
                        isNullishConst[inst.result] = 1;
                    } else if (inst.op == il::Op::CreateObject) {
                        isCreateObjVal[inst.result] = 1;
                    }
                }
            }
        }

        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.keyIndex >= numKeys) continue;
                const uint32_t k = inst.keyIndex;
                if (inst.op == il::Op::MethodDef) {
                    if (inst.operands.size() >= 2 && inst.operands[1] < fn.valueCount &&
                        funcValCallee[inst.operands[1]] >= 0) {
                        const auto cIdx = static_cast<uint32_t>(funcValCallee[inst.operands[1]]);
                        targets[k].push_back(cIdx);
                        methodFnToKey[cIdx] = k;
                    } else {
                        valid[k] = 0;
                    }
                } else if (inst.op == il::Op::AccessorDef || inst.op == il::Op::PropDelete ||
                           inst.op == il::Op::DefineOwnAttr) {
                    if (inst.op == il::Op::DefineOwnAttr && !inst.operands.empty() &&
                        inst.operands[0] < fn.valueCount && isCreateObjVal[inst.operands[0]]) {
                        continue;
                    }
                    valid[k] = 0;
                    keyMutatedByPropSet[k] = 1;
                } else if (inst.op == il::Op::PropSet) {
                    if (!inst.operands.empty() && inst.operands[0] < fn.valueCount &&
                        isCreateObjVal[inst.operands[0]]) {
                        continue;
                    }
                    keyMutatedByPropSet[k] = 1;
                    if (inst.operands.size() < 2 || inst.operands[1] >= fn.valueCount) {
                        valid[k] = 0;
                        continue;
                    }
                    const il::ValueId val = inst.operands[1];
                    if (funcValCallee[val] >= 0) {
                        targets[k].push_back(static_cast<uint32_t>(funcValCallee[val]));
                    } else if (isNullishConst[val]) {
                        // Null/undefined is not callable
                    } else if (fn.needsThis && inst.operands[0] == thisParamId &&
                               val >= fn.firstSourceParam() && val < fn.params.size()) {
                        forwarded.push_back(
                            ForwardedSetter{static_cast<uint32_t>(fnIdx), val, k});
                    } else {
                        valid[k] = 0;
                    }
                }
            }
        }
    }

    for (const auto& fw : forwarded) {
        if (!valid[fw.targetKey]) continue;
        const auto it = methodFnToKey.find(fw.setterFnIdx);
        if (it == methodFnToKey.end() || keyMutatedByPropSet[it->second]) {
            valid[fw.targetKey] = 0;
            continue;
        }
        const uint32_t setterKey = it->second;
        const auto& setterFn = module.functions[fw.setterFnIdx];
        const size_t argOffset = 1 + (fw.paramIdx - setterFn.firstSourceParam());

        for (size_t fnIdx = 0; fnIdx < numFns && valid[fw.targetKey]; ++fnIdx) {
            const auto& fn = module.functions[fnIdx];
            const auto& funcValCallee = allFuncValCallee[fnIdx];
            for (const auto& block : fn.blocks) {
                for (const auto& inst : block.instructions) {
                    if (inst.op == il::Op::MethodCall && inst.keyIndex == setterKey) {
                        if (argOffset >= inst.operands.size() ||
                            inst.operands[argOffset] >= fn.valueCount ||
                            funcValCallee[inst.operands[argOffset]] < 0) {
                            valid[fw.targetKey] = 0;
                            break;
                        }
                        targets[fw.targetKey].push_back(
                            static_cast<uint32_t>(funcValCallee[inst.operands[argOffset]]));
                    }
                }
                if (!valid[fw.targetKey]) break;
            }
        }
    }

    for (uint32_t k = 0; k < numKeys; ++k) {
        if (!valid[k]) {
            targets[k].clear();
        } else if (!targets[k].empty()) {
            std::sort(targets[k].begin(), targets[k].end());
            targets[k].erase(std::unique(targets[k].begin(), targets[k].end()), targets[k].end());
        }
    }

    return targets;
}

bool isSealedDirectMethodCall(const il::Instruction& inst, const il::Module& module,
                              const DirectMethodTable* directMethods, bool protoPatchFree,
                              const std::vector<uint8_t>& isMutatedMethodKey) {
    if (!protoPatchFree || directMethods == nullptr) return false;
    if (inst.op != il::Op::MethodCall) return false;
    if (inst.directTarget == il::Instruction::kNoDirectTarget ||
        inst.directTarget >= module.functions.size()) {
        return false;
    }
    if (inst.keyIndex >= isMutatedMethodKey.size() || isMutatedMethodKey[inst.keyIndex]) {
        return false;
    }
    return directMethods->isMethodSealedOnSubclasses(inst.icIndex);
}

}  // namespace bronze::lower
