#include "lower/loop_prop_analysis.h"

#include "abi/bronze_abi.h"

namespace bronze::lower {

// Can this instruction change a property some earlier read already answered?
// Written the SAFE WAY ROUND, like `canThrow` in il/effects.cpp: the cases
// below write no property and run no user code; everything else is impure.
bool isImpureForLoopPropHoist(const il::Instruction& inst) {
    switch (inst.op) {
        case il::Op::ConstF64: case il::Op::ConstI32: case il::Op::ConstBool:
        case il::Op::ConstUndefined: case il::Op::ConstNull: case il::Op::ConstBigInt:
        case il::Op::CmpLt: case il::Op::CmpGt: case il::Op::CmpLe:
        case il::Op::CmpGe: case il::Op::CmpEq: case il::Op::CmpNe:
        case il::Op::NumTruthy: case il::Op::StrictEq: case il::Op::IsNullish:
        case il::Op::IsNumber: case il::Op::IsDenseArray: case il::Op::TypeOf:
        case il::Op::Box: case il::Op::MathUnary: case il::Op::CensusRecord:
        case il::Op::KeepAlive: case il::Op::PinGuard: case il::Op::PatternCheck:
        case il::Op::PrivateMisuse: case il::Op::CreateObject: case il::Op::CreateArray:
        case il::Op::CreateFunction: case il::Op::FunctionRef: case il::Op::EnvCreate:
        case il::Op::PrivateNew: case il::Op::ModuleNamespace: case il::Op::TemplateCached:
        case il::Op::TemplateObject: case il::Op::CoroMode: case il::Op::GetNewTarget:
        case il::Op::ImportMeta: case il::Op::ExcTake: case il::Op::EnvGet:
        case il::Op::EnvGetTdz: case il::Op::ModuleEnvGet: case il::Op::GlobalGet:
        case il::Op::ResolveName: case il::Op::PropGet: case il::Op::ElemGet:
        case il::Op::ElemGetTyped: case il::Op::SuperGet: case il::Op::PrivateHas:
        case il::Op::PrivateGet: case il::Op::ArrayAppend: case il::Op::ArrayAppendHole:
        case il::Op::Print: case il::Op::PrintErr: case il::Op::PrintSpread:
        case il::Op::PrintSpreadErr: case il::Op::ConcatEnd: case il::Op::Ret:
        case il::Op::Jump: case il::Op::Branch: case il::Op::Add:
        case il::Op::Sub: case il::Op::Neg: case il::Op::Mul:
        case il::Op::Div: case il::Op::Mod: case il::Op::Pow:
        case il::Op::BitAnd: case il::Op::BitOr: case il::Op::BitXor:
        case il::Op::MathImul: case il::Op::Shl: case il::Op::Shr:
        case il::Op::UShr: case il::Op::BitNot: case il::Op::Unbox:
        case il::Op::ToInt32: case il::Op::ToNumeric: case il::Op::NumericStep:
        case il::Op::ToStr: case il::Op::ConcatBegin: case il::Op::ConcatAppend:
        case il::Op::RelLt: case il::Op::RelGt: case il::Op::RelLe:
        case il::Op::RelGe: case il::Op::LooseEq: case il::Op::InstanceOf:
        case il::Op::In: case il::Op::ObjectKeys: case il::Op::ForInKeys:
            return false;
        default:
            return true;
    }
}

bool isArrayIndexKey(const std::string& key) {
    if (key.empty() || key.size() > 10 || (key.size() > 1 && key[0] == '0')) return false;
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
    for (const auto& blk : calleeFn.blocks) if (blk.handler != il::kNoBlock) return info;
    if (!calleeFn.needsThis) return info;
    // A property is forwarded to the call's argument at its parameter's
    // position. A rest parameter is no one argument but an array of all the
    // rest, so `this.items = items` read back as the first of them.
    if (calleeFn.hasRestParam) return info;
    const il::ValueId thisVal = calleeFn.needsEnv ? 1u : 0u;
    const size_t firstSource = calleeFn.firstSourceParam();

    for (const auto& blk : calleeFn.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == il::Op::PropSet) {
                if (inst.operands.empty() || inst.operands[0] != thisVal) return info;
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
                        if (!replaced) info.propArgMap.push_back({inst.keyIndex, argIdx});
                    }
                }
                continue;
            }
            if (isImpureForLoopPropHoist(inst)) return info;
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
                    if ((defInst.op == il::Op::FunctionRef ||
                         defInst.op == il::Op::CreateFunction) &&
                        defInst.calleeIndex < module.functions.size()) {
                        return defInst.calleeIndex;
                    }
                    if (defInst.op == il::Op::EnvGet || defInst.op == il::Op::EnvGetTdz) {
                        const uint32_t slotIdx = defInst.envIndex;
                        for (const auto& f : module.functions) {
                            const bool isModSeg = f.name.rfind("main.seg", 0) == 0;
                            if (!f.isEntryPoint && !isModSeg) continue;
                            const std::vector<DefSite> mDefs = computeDefSites(f);
                            for (const auto& mb : f.blocks) {
                                for (const auto& mInst : mb.instructions) {
                                    if (mInst.op == il::Op::EnvSet && mInst.envDepth == 0 &&
                                        mInst.envIndex == slotIdx && mInst.operands.size() >= 2) {
                                        const il::ValueId v = mInst.operands[1];
                                        if (v < mDefs.size() && mDefs[v].block != il::kNoBlock &&
                                            mDefs[v].index <
                                                f.blocks[mDefs[v].block].instructions.size()) {
                                            const auto& di =
                                                f.blocks[mDefs[v].block].instructions[mDefs[v].index];
                                            if ((di.op == il::Op::CreateFunction ||
                                                 di.op == il::Op::FunctionRef) &&
                                                di.calleeIndex < module.functions.size()) {
                                                return di.calleeIndex;
                                            }
                                        }
                                    }
                                }
                            }
                        }
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
    return inst.op == il::Op::Construct || inst.op == il::Op::ConstructSpread ||
           inst.op == il::Op::CreateObject || inst.op == il::Op::CreateArray;
}

bool isNonEscapingLocalObject(il::ValueId val, const il::Function& fn,
                             const std::vector<DefSite>& defs) {
    if (val >= defs.size()) return false;
    const auto& def = defs[val];
    if (def.block == il::kNoBlock || def.block >= fn.blocks.size()) return false;
    const auto& b = fn.blocks[def.block];
    if (def.index >= b.instructions.size()) return false;
    const auto& defInst = b.instructions[def.index];
    if (defInst.op != il::Op::CreateObject && defInst.op != il::Op::CreateArray) return false;

    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            for (il::ValueId arg : inst.target.args) if (arg == val) return false;
            for (il::ValueId arg : inst.elseTarget.args) if (arg == val) return false;
            for (size_t opIdx = 0; opIdx < inst.operands.size(); ++opIdx) {
                if (inst.operands[opIdx] != val) continue;
                switch (inst.op) {
                    case il::Op::PropGet:
                    case il::Op::ElemGet:
                    case il::Op::ElemGetTyped:
                    case il::Op::PropSet:
                    case il::Op::ElemSet:
                    case il::Op::ElemSetTyped:
                    case il::Op::InstanceOf:
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

namespace {

bool isProvablyNonNullishImpl(il::ValueId val, const il::Function& fn,
                              const std::vector<DefSite>& defs,
                              const il::Module& module,
                              std::vector<std::optional<SafeConstructorInfo>>& ctorCache,
                              int depth) {
    if (depth <= 0 || val >= defs.size()) return false;
    const auto& def = defs[val];
    if (def.block == il::kNoBlock || def.block >= fn.blocks.size()) {
        // A source parameter of a non-exported, non-entry function where every
        // call/construct site passes a provably non-nullish argument.
        if (!fn.isExported && !fn.isEntryPoint && val < fn.params.size() &&
            val >= fn.firstSourceParam()) {
            size_t fnIdx = SIZE_MAX;
            for (size_t i = 0; i < module.functions.size(); ++i) {
                if (&module.functions[i] == &fn || module.functions[i].name == fn.name) {
                    fnIdx = i;
                    break;
                }
            }
            if (fnIdx == SIZE_MAX) return false;
            const size_t srcIdx = val - fn.firstSourceParam();

            if ((fn.descFlags & BRONZE_FN_DESC_CONSTRUCTOR) != 0) {
                uint32_t ctorCount = 0;
                for (const auto& callerFn : module.functions) {
                    const std::vector<DefSite> callerDefs = computeDefSites(callerFn);
                    for (const auto& blk : callerFn.blocks) {
                        for (const auto& inst : blk.instructions) {
                            if (inst.op == il::Op::Construct || inst.op == il::Op::SuperCall) {
                                if (findCalleeIndex(inst, callerFn, callerDefs, module) != fnIdx) {
                                    continue;
                                }
                                const size_t opOff = (inst.op == il::Op::Construct) ? 1u : 2u;
                                if (opOff + srcIdx >= inst.operands.size() ||
                                    !isProvablyNonNullishImpl(inst.operands[opOff + srcIdx],
                                                              callerFn, callerDefs, module,
                                                              ctorCache, depth - 1)) {
                                    return false;
                                }
                                ++ctorCount;
                            } else if ((inst.op == il::Op::ConstructSpread ||
                                        inst.op == il::Op::SuperCallSpread) &&
                                       findCalleeIndex(inst, callerFn, callerDefs, module) == fnIdx) {
                                return false;
                            }
                        }
                    }
                }
                return ctorCount > 0;
            }

            uint32_t callCount = 0;
            for (const auto& callerFn : module.functions) {
                for (const auto& blk : callerFn.blocks) {
                    for (const auto& inst : blk.instructions) {
                        if ((inst.op == il::Op::CreateFunction || inst.op == il::Op::FunctionRef) &&
                            inst.calleeIndex == fnIdx && inst.result != il::kNoValue) {
                            for (const auto& b2 : callerFn.blocks) {
                                for (const auto& u : b2.instructions) {
                                    for (il::ValueId op : u.operands) {
                                        if (op != inst.result) continue;
                                        const bool isEnv = u.op == il::Op::EnvSet && u.operands.size() >= 2 && u.operands[1] == inst.result;
                                        const bool isProp = callerFn.isEntryPoint && u.op == il::Op::PropSet && u.operands.size() >= 2 && u.operands[1] == inst.result;
                                        if (!isEnv && !isProp) return false;
                                        for (const auto& f3 : module.functions) {
                                            for (const auto& b3 : f3.blocks) {
                                                for (const auto& i3 : b3.instructions) {
                                                    if (isEnv && (i3.op == il::Op::EnvGet || i3.op == il::Op::EnvGetTdz) && i3.envIndex == u.envIndex) return false;
                                                    if (isProp && (i3.op == il::Op::PropGet || i3.op == il::Op::MethodCall || i3.op == il::Op::GlobalGet) && i3.keyIndex == u.keyIndex) return false;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                std::vector<DefSite> callerDefs;
                bool defsReady = false;
                for (const auto& blk : callerFn.blocks) {
                    for (const auto& inst : blk.instructions) {
                        if (inst.op == il::Op::Call && inst.calleeIndex == fnIdx) {
                            if (val >= inst.operands.size()) return false;
                            if (!defsReady) {
                                callerDefs = computeDefSites(callerFn);
                                defsReady = true;
                            }
                            if (!isProvablyNonNullishImpl(inst.operands[val], callerFn, callerDefs,
                                                          module, ctorCache, depth - 1)) {
                                return false;
                            }
                            ++callCount;
                        }
                    }
                }
            }
            return callCount > 0;
        }
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
        case il::Op::CoroStart:
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

        case il::Op::Call: {
            if (inst.calleeIndex < module.functions.size()) {
                const auto& calleeFn = module.functions[inst.calleeIndex];
                if (calleeFn.blocks.size() == 1 && !calleeFn.blocks[0].instructions.empty()) {
                    const auto& retInst = calleeFn.blocks[0].instructions.back();
                    if (retInst.op == il::Op::Ret && !retInst.operands.empty()) {
                        const std::vector<DefSite> cDefs = computeDefSites(calleeFn);
                        return isProvablyNonNullishImpl(retInst.operands[0], calleeFn, cDefs,
                                                        module, ctorCache, depth - 1);
                    }
                }
            }
            return false;
        }

        case il::Op::GlobalGet:
            if (inst.keyIndex < module.keyConstants.size()) {
                const auto& name = module.keyConstants[inst.keyIndex];
                if (name == "Math" || name == "Object" || name == "Array" ||
                    name == "String" || name == "Number" || name == "Boolean") {
                    return true;
                }
            }
            return false;

        case il::Op::PropGet: {
            if (!il::provenStaticSlot(inst) ||
                inst.keyIndex >= module.keyConstants.size()) {
                return false;
            }
            uint32_t writeCount = 0;
            for (const auto& f : module.functions) {
                std::vector<DefSite> fDefs;
                bool defsReady = false;
                for (const auto& b : f.blocks) {
                    for (const auto& i : b.instructions) {
                        if ((i.op == il::Op::PropSet || i.op == il::Op::DefineOwnAttr) &&
                            i.keyIndex == inst.keyIndex) {
                            if (il::provenStaticSlot(i) &&
                                i.staticSlot != inst.staticSlot) {
                                continue;
                            }
                            if (i.operands.size() < 2) return false;
                            if (!defsReady) {
                                fDefs = computeDefSites(f);
                                defsReady = true;
                            }
                            const il::ValueId recvVal = i.operands[0];
                            if (recvVal < fDefs.size() && fDefs[recvVal].block != il::kNoBlock &&
                                fDefs[recvVal].index <
                                    f.blocks[fDefs[recvVal].block].instructions.size() &&
                                f.blocks[fDefs[recvVal].block].instructions[fDefs[recvVal].index].op ==
                                    il::Op::CreateObject) {
                                continue;
                            }
                            if (!isProvablyNonNullishImpl(i.operands[1], f, fDefs, module,
                                                          ctorCache, depth - 1)) {
                                return false;
                            }
                            ++writeCount;
                        }
                    }
                }
            }
            return writeCount > 0;
        }

        case il::Op::EnvGet:
        case il::Op::EnvGetTdz: {
            if (inst.envDepth != 0 || inst.operands.empty()) return false;
            const il::ValueId envVal = inst.operands[0];
            if (envVal >= defs.size()) return false;
            const auto& envDef = defs[envVal];
            if (envDef.block == il::kNoBlock || envDef.block >= fn.blocks.size()) return false;
            const auto& envBlk = fn.blocks[envDef.block];
            if (envDef.index >= envBlk.instructions.size()) return false;
            const auto& envInst = envBlk.instructions[envDef.index];
            const uint32_t slotIdx = inst.envIndex;

            if (envInst.op == il::Op::ModuleEnvGet) {
                const bool isTdzGet = (inst.op == il::Op::EnvGetTdz);
                const il::Function* initFn = nullptr;
                il::ValueId initVal = il::kNoValue;
                uint32_t envSetCount = 0;

                for (const auto& f : module.functions) {
                    const std::vector<DefSite> fDefs = computeDefSites(f);
                    const bool isModInit = f.isEntryPoint || f.name.rfind("main.seg", 0) == 0;
                    il::ValueId modEnvReg = il::kNoValue;
                    bool hasTdzInit = false;
                    bool callBeforeInit = false;

                    for (const auto& b : f.blocks) {
                        for (const auto& i : b.instructions) {
                            if (i.op == il::Op::ModuleEnvSet && !i.operands.empty()) {
                                modEnvReg = i.operands[0];
                            } else if (i.op == il::Op::EnvInitTdz && i.envDepth == 0 &&
                                       i.envIndex == slotIdx && !i.operands.empty() &&
                                       i.operands[0] == modEnvReg) {
                                hasTdzInit = true;
                            } else if (i.op == il::Op::Call || i.op == il::Op::DynamicCall ||
                                       i.op == il::Op::MethodCall) {
                                if (initVal == il::kNoValue) callBeforeInit = true;
                            }
                            if (i.op != il::Op::EnvSet || i.envIndex != slotIdx ||
                                i.operands.size() < 2) {
                                continue;
                            }
                            if (i.envDepth == 0) {
                                const il::ValueId targetEnv = i.operands[0];
                                bool isModEnv = (f.isEntryPoint && targetEnv == modEnvReg);
                                if (!isModEnv && targetEnv < fDefs.size() &&
                                    fDefs[targetEnv].block != il::kNoBlock &&
                                    fDefs[targetEnv].block < f.blocks.size()) {
                                    const auto& eb = f.blocks[fDefs[targetEnv].block];
                                    if (fDefs[targetEnv].index < eb.instructions.size() &&
                                        eb.instructions[fDefs[targetEnv].index].op ==
                                            il::Op::ModuleEnvGet) {
                                        isModEnv = true;
                                    }
                                }
                                if (isModEnv) {
                                    ++envSetCount;
                                    if (isModInit && initVal == il::kNoValue &&
                                        (isTdzGet || hasTdzInit || !callBeforeInit)) {
                                        initFn = &f;
                                        initVal = i.operands[1];
                                    }
                                }
                            } else {
                                ++envSetCount;
                            }
                            if (envSetCount > 1) return false;
                        }
                    }
                }
                if (envSetCount != 1 || initFn == nullptr || initVal == il::kNoValue) {
                    return false;
                }
                const std::vector<DefSite> initDefs = computeDefSites(*initFn);
                return isProvablyNonNullishImpl(initVal, *initFn, initDefs, module, ctorCache,
                                                depth - 1);
            }
            return false;
        }

        default:
            return false;
    }
}

}  // namespace

bool isProvablyNonNullish(il::ValueId val, const il::Function& fn,
                          const std::vector<DefSite>& defs,
                          const il::Module& module,
                          std::vector<std::optional<SafeConstructorInfo>>& ctorCache) {
    return isProvablyNonNullishImpl(val, fn, defs, module, ctorCache, 6);
}

bool isArrayMutatingMethod(const std::string& name) {
    return name == "push" || name == "pop" || name == "shift" ||
           name == "unshift" || name == "splice" || name == "reverse" ||
           name == "sort" || name == "fill" || name == "copyWithin" ||
           name == "transfer" || name == "transferToFixedLength" || name == "resize";
}

bool isArrayReadOnlyMethod(const std::string& name) {
    return name == "indexOf" || name == "lastIndexOf" || name == "includes" ||
           name == "slice" || name == "join" || name == "concat" ||
           name == "at" || name == "toString" || name == "isArray";
}

}  // namespace bronze::lower
