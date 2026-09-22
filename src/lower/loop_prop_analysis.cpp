#include "lower/loop_prop_analysis.h"

#include <algorithm>

namespace bronze::lower {

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

bool isNonEscapingLocalObject(il::ValueId val, const il::Function& fn,
                             const std::vector<DefSite>& defs) {
    if (val >= defs.size()) return false;
    const auto& def = defs[val];
    if (def.block == il::kNoBlock || def.block >= fn.blocks.size()) return false;
    const auto& b = fn.blocks[def.block];
    if (def.index >= b.instructions.size()) return false;
    const auto& defInst = b.instructions[def.index];
    if (defInst.op != il::Op::CreateObject && defInst.op != il::Op::CreateArray) {
        return false;
    }

    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            for (il::ValueId arg : inst.target.args) {
                if (arg == val) return false;
            }
            for (il::ValueId arg : inst.elseTarget.args) {
                if (arg == val) return false;
            }
            for (size_t opIdx = 0; opIdx < inst.operands.size(); ++opIdx) {
                if (inst.operands[opIdx] != val) continue;

                switch (inst.op) {
                    case il::Op::PropGet:
                        if (opIdx == 0) continue;
                        return false;

                    case il::Op::ElemGet:
                    case il::Op::ElemGetTyped:
                        if (opIdx == 0) continue;
                        return false;

                    case il::Op::PropSet:
                        if (opIdx == 0) continue;
                        return false;

                    case il::Op::ElemSet:
                    case il::Op::ElemSetTyped:
                        if (opIdx == 0) continue;
                        return false;

                    case il::Op::StrictEq:
                    case il::Op::LooseEq:
                    case il::Op::CmpEq:
                    case il::Op::CmpNe:
                    case il::Op::TypeOf:
                    case il::Op::IsNullish:
                    case il::Op::IsNumber:
                    case il::Op::IsDenseArray:
                        continue;

                    case il::Op::InstanceOf:
                        if (opIdx == 0) continue;
                        return false;

                    case il::Op::In:
                        if (opIdx == 1) continue;
                        return false;

                    default:
                        return false;
                }
            }
        }
    }
    return true;
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

bool isProvablyNonNullish(il::ValueId val, const il::Function& fn,
                          const std::vector<DefSite>& defs,
                          const il::Module& module,
                          std::vector<std::optional<SafeConstructorInfo>>& /*ctorCache*/) {
    if (val >= defs.size()) return false;
    const auto& def = defs[val];
    if (def.block == il::kNoBlock || def.block >= fn.blocks.size()) {
        return false;
    }
    const auto& blk = fn.blocks[def.block];
    if (def.index >= blk.instructions.size()) return false;
    const auto& inst = blk.instructions[def.index];

    if (inst.type == il::Type::Bool || inst.type == il::Type::I32 ||
        inst.type == il::Type::F64 || inst.type == il::Type::Str) {
        return true;
    }

    switch (inst.op) {
        case il::Op::ConstF64:
        case il::Op::ConstI32:
        case il::Op::ConstBool:
        case il::Op::ConstBigInt:
        case il::Op::CreateObject:
        case il::Op::CreateArray:
        case il::Op::CreateFunction:
        case il::Op::CreateGeneratorObject:
        case il::Op::CreateAsyncGeneratorObject:
        case il::Op::CreateAsyncMachine:
        case il::Op::ModuleNamespace:
        case il::Op::ObjectKeys:
        case il::Op::ForInKeys:
        case il::Op::ToStr:
        case il::Op::TypeOf:
        case il::Op::ConcatBegin:
        case il::Op::ConcatAppend:
        case il::Op::ConcatEnd:
            return true;

        case il::Op::Box:
            return inst.boxType != il::Type::Void;

        case il::Op::Construct:
        case il::Op::ConstructSpread:
            return true;

        case il::Op::GlobalGet:
            if (inst.keyIndex < module.keyConstants.size()) {
                const auto& name = module.keyConstants[inst.keyIndex];
                if (name == "Math" || name == "Object" || name == "Array" ||
                    name == "String" || name == "Number" || name == "Boolean") {
                    return true;
                }
            }
            return false;

        default:
            return false;
    }
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
                        if (!isNonEscapingLocalObject(recv, fn, defs)) return true;
                    }
                    if (keyIndex < eff.writtenKeys.size() && eff.writtenKeys[keyIndex]) {
                        for (il::ValueId op : inst.operands) {
                            if (op == recv) return true;
                        }
                        if (!isNonEscapingLocalObject(recv, fn, defs)) return true;
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
                        if (!isNonEscapingLocalObject(recv, fn, defs)) return true;
                    }
                    if (keyIndex < eff.writtenKeys.size() && eff.writtenKeys[keyIndex]) {
                        for (il::ValueId op : inst.operands) {
                            if (op == recv) return true;
                        }
                        if (!isNonEscapingLocalObject(recv, fn, defs)) return true;
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

}  // namespace bronze::lower
