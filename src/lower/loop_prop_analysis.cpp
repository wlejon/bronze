#include "lower/loop_prop_analysis.h"

#include <algorithm>
#include <iterator>

#include "abi/bronze_abi.h"
#include "lower/direct_method_table.h"
#include "lower/inline_small.h"

namespace bronze::lower {

// Can this instruction change a property some earlier read already answered?
// Written the SAFE WAY ROUND, like `canThrow` in il/effects.cpp: the cases
// below are the ones that write no property and run no user code (under the
// two assumptions named below), and everything else is impure. An op the list does not name — including one added
// tomorrow — costs a forwarding, never a stale read. The other way round,
// `object.spread` (which defines the source's keys on its target and runs the
// source's getters) read as pure, and `{ from: -1, ...p }.from` answered -1.
//
// Property READS are on the pure side, and that is the passes' standing
// assumption rather than a fact about the op: a read of a key some module
// accessor is defined under is a call, and the callers rule those keys out by
// `isAccessorKey` where they consult the table.
bool isImpureForLoopPropHoist(const il::Instruction& inst) {
    switch (inst.op) {
        // Constants, machine-number compares and tag tests: no call at all.
        case il::Op::ConstF64:
        case il::Op::ConstI32:
        case il::Op::ConstBool:
        case il::Op::ConstUndefined:
        case il::Op::ConstNull:
        case il::Op::ConstBigInt:
        case il::Op::CmpLt:
        case il::Op::CmpGt:
        case il::Op::CmpLe:
        case il::Op::CmpGe:
        case il::Op::CmpEq:
        case il::Op::CmpNe:
        case il::Op::NumTruthy:
        case il::Op::StrictEq:
        case il::Op::IsNullish:
        case il::Op::IsNumber:
        case il::Op::IsDenseArray:
        case il::Op::TypeOf:
        case il::Op::Box:
        case il::Op::MathUnary:
        case il::Op::CensusRecord:
        case il::Op::KeepAlive:
        // Refusals: each raises or answers, and none reaches user code.
        case il::Op::PinGuard:
        case il::Op::PatternCheck:
        case il::Op::PrivateMisuse:
        // Allocation, and reads of what the frame or module already holds.
        case il::Op::CreateObject:
        case il::Op::CreateArray:
        case il::Op::CreateFunction:
        case il::Op::FunctionRef:
        case il::Op::EnvCreate:
        case il::Op::PrivateNew:
        case il::Op::ModuleNamespace:
        case il::Op::TemplateCached:
        case il::Op::TemplateObject:
        case il::Op::CoroMode:
        case il::Op::GetNewTarget:
        case il::Op::ImportMeta:
        case il::Op::ExcTake:
        case il::Op::EnvGet:
        case il::Op::EnvGetTdz:
        case il::Op::ModuleEnvGet:
        case il::Op::GlobalGet:
        case il::Op::ResolveName:
        // Property reads (see above). `private.get` has no accessor step: that
        // is resolved in lowering into a call.
        case il::Op::PropGet:
        case il::Op::ElemGet:
        case il::Op::ElemGetTyped:
        case il::Op::SuperGet:
        case il::Op::PrivateHas:
        case il::Op::PrivateGet:
        // The elements of an array literal under construction: the array is
        // not yet visible to anything that could have read it.
        case il::Op::ArrayAppend:
        case il::Op::ArrayAppendHole:
        // console.log never runs user code: inspect does not call a getter.
        case il::Op::Print:
        case il::Op::PrintErr:
        case il::Op::PrintSpread:
        case il::Op::PrintSpreadErr:
        // Clears the accumulator's bit and reads nothing.
        case il::Op::ConcatEnd:
        // Leaving the block: there is nothing after it in the block to be stale.
        case il::Op::Ret:
        case il::Op::Jump:
        case il::Op::Branch:
        // Coercions and protocol tests. On a boxed operand each of these can
        // reach a hook the language calls implicitly — ToPrimitive's `valueOf`
        // / `toString` / `Symbol.toPrimitive`, `Symbol.hasInstance`, a Proxy
        // trap — and those hooks are ASSUMED not to write a property, the same
        // standing assumption the reads above carry. It is an assumption and
        // not a fact (cases/blocked/prop_reread_across_to_primitive.js), kept
        // because the alternative is that every boxed `+` ends forwarding and
        // hoisting on the dynamic path. What the assumption does NOT cover is
        // an op whose own job is to run the source's getters or iterator —
        // object spread and rest, array spread, iteration — which fall to the
        // default below.
        case il::Op::Add:
        case il::Op::Sub:
        case il::Op::Neg:
        case il::Op::Mul:
        case il::Op::Div:
        case il::Op::Mod:
        case il::Op::Pow:
        case il::Op::BitAnd:
        case il::Op::BitOr:
        case il::Op::BitXor:
        case il::Op::MathImul:
        case il::Op::Shl:
        case il::Op::Shr:
        case il::Op::UShr:
        case il::Op::BitNot:
        case il::Op::Unbox:
        case il::Op::ToInt32:
        case il::Op::ToNumeric:
        case il::Op::NumericStep:
        case il::Op::ToStr:
        case il::Op::ConcatBegin:
        case il::Op::ConcatAppend:
        case il::Op::RelLt:
        case il::Op::RelGt:
        case il::Op::RelLe:
        case il::Op::RelGe:
        case il::Op::LooseEq:
        case il::Op::InstanceOf:
        case il::Op::In:
        case il::Op::ObjectKeys:
        case il::Op::ForInKeys:
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
                                        if (u.op != il::Op::EnvSet || u.operands.size() < 2 ||
                                            u.operands[1] != inst.result) {
                                            return false;
                                        }
                                        for (const auto& f3 : module.functions) {
                                            for (const auto& b3 : f3.blocks) {
                                                for (const auto& i3 : b3.instructions) {
                                                    if ((i3.op == il::Op::EnvGet ||
                                                         i3.op == il::Op::EnvGetTdz) &&
                                                        i3.envIndex == u.envIndex) {
                                                        return false;
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
            if (inst.staticSlot == il::Instruction::kNoStaticSlot ||
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
                            if (i.staticSlot != il::Instruction::kNoStaticSlot &&
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
           name == "sort" || name == "fill" || name == "copyWithin";
}

bool isArrayReadOnlyMethod(const std::string& name) {
    return name == "indexOf" || name == "lastIndexOf" || name == "includes" ||
           name == "slice" || name == "join" || name == "concat" ||
           name == "at" || name == "toString" || name == "isArray";
}

namespace {

bool isBuiltinPureConstruct(const il::Instruction& inst, const il::Function& fn,
                            const std::vector<DefSite>& defs, const il::Module& module) {
    if (inst.op != il::Op::Construct || inst.operands.empty()) return false;
    const il::ValueId cVal = inst.operands[0];
    if (cVal >= defs.size() || defs[cVal].block == il::kNoBlock ||
        defs[cVal].index >= fn.blocks[defs[cVal].block].instructions.size()) {
        return false;
    }
    const auto& di = fn.blocks[defs[cVal].block].instructions[defs[cVal].index];
    if (di.op != il::Op::GlobalGet || di.keyIndex >= module.keyConstants.size()) return false;
    const std::string& n = module.keyConstants[di.keyIndex];
    return n == "Float32Array" || n == "Float64Array" || n == "Int32Array" ||
           n == "Uint32Array" || n == "Int16Array" || n == "Uint16Array" ||
           n == "Int8Array" || n == "Uint8Array" || n == "Uint8ClampedArray" ||
           n == "Array" || n == "Object" || n == "Error" ||
           n == "TypeError" || n == "RangeError";
}

bool isMathPropCall(const il::Instruction& inst, const il::Function& fn,
                    const std::vector<DefSite>& defs, const il::Module& module) {
    if (inst.operands.empty()) return false;
    const il::ValueId cVal = inst.operands[0];
    if (cVal >= defs.size() || defs[cVal].block == il::kNoBlock ||
        defs[cVal].index >= fn.blocks[defs[cVal].block].instructions.size()) {
        return false;
    }
    const auto& di = fn.blocks[defs[cVal].block].instructions[defs[cVal].index];
    return di.op == il::Op::PropGet && !di.operands.empty() &&
           isMathObject(di.operands[0], fn, defs, module);
}

}  // namespace

std::vector<FunctionEffects> computeModuleEffects(
    const il::Module& module,
    const std::vector<uint8_t>& isAccessorKey,
    std::vector<std::optional<SafeConstructorInfo>>& ctorCache,
    uint32_t lengthKeyIndex,
    const DirectMethodTable* directMethods) {
    std::vector<FunctionEffects> effects(module.functions.size());
    const bool protoPatchFree = isModulePrototypePatchFree(module);
    const std::vector<uint8_t> isMutatedMethodKey = computeMutatedMethodKeyMask(module);
    const std::vector<std::vector<uint32_t>> closedKeyTargets =
        protoPatchFree ? computeClosedKeyTargets(module)
                       : std::vector<std::vector<uint32_t>>{};

    for (size_t f = 0; f < module.functions.size(); ++f) {
        const auto& fn = module.functions[f];
        std::vector<DefSite> defs = computeDefSites(fn);
        const bool isCtor = (fn.descFlags & BRONZE_FN_DESC_CONSTRUCTOR) != 0;
        const il::ValueId thisVal = fn.needsEnv ? 1u : 0u;

        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::PropSet) {
                    const bool isAcc = inst.keyIndex < isAccessorKey.size() &&
                                       isAccessorKey[inst.keyIndex] &&
                                       !(protoPatchFree &&
                                         inst.staticSlot != il::Instruction::kNoStaticSlot);
                    bool isFreshCtorThis = false;
                    if (isCtor && !inst.operands.empty()) {
                        const il::ValueId t = inst.operands[0];
                        if (fn.needsThis && t == thisVal) {
                            isFreshCtorThis = true;
                        } else if (t < defs.size() && defs[t].block != il::kNoBlock &&
                                   defs[t].index < fn.blocks[defs[t].block].instructions.size() &&
                                   fn.blocks[defs[t].block].instructions[defs[t].index].op ==
                                       il::Op::SuperCall) {
                            isFreshCtorThis = true;
                        }
                    }
                    if (isAcc) {
                        effects[f].hasUnresolvedCalls = true;
                    } else if (!isFreshCtorThis && inst.keyIndex < module.keyConstants.size()) {
                        effects[f].writtenKeys.push_back(inst.keyIndex);
                    }
                    if (!isFreshCtorThis &&
                        (inst.keyIndex == lengthKeyIndex ||
                         (inst.keyIndex < module.keyConstants.size() &&
                          isArrayIndexKey(module.keyConstants[inst.keyIndex])))) {
                        effects[f].mutatesLength = true;
                    }
                } else if (inst.op == il::Op::ElemSet || inst.op == il::Op::ElemSetTyped) {
                    effects[f].mutatesLength = true;
                } else if (inst.op == il::Op::MethodCall) {
                    if (!inst.operands.empty() && isMathObject(inst.operands[0], fn, defs, module)) {
                        // Pure Math method
                    } else if (isSealedDirectMethodCall(inst, module, directMethods, protoPatchFree,
                                                        isMutatedMethodKey)) {
                        effects[f].directCallees.push_back(inst.directTarget);
                    } else if (protoPatchFree && inst.keyIndex < closedKeyTargets.size() &&
                               !closedKeyTargets[inst.keyIndex].empty()) {
                        for (uint32_t tgt : closedKeyTargets[inst.keyIndex]) {
                            effects[f].directCallees.push_back(tgt);
                        }
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
                } else if (inst.op == il::Op::Call || inst.op == il::Op::DynamicCall) {
                    const uint32_t callee = findCalleeIndex(inst, fn, defs, module);
                    if (callee != UINT32_MAX && callee < module.functions.size()) {
                        effects[f].directCallees.push_back(callee);
                    } else if (isMathPropCall(inst, fn, defs, module)) {
                        // Pure Math function call (e.g. const cos = Math.cos; cos(x))
                    } else if (protoPatchFree && !inst.operands.empty() &&
                               inst.operands[0] < defs.size() &&
                               defs[inst.operands[0]].block != il::kNoBlock &&
                               defs[inst.operands[0]].index <
                                   fn.blocks[defs[inst.operands[0]].block].instructions.size()) {
                        const auto& di = fn.blocks[defs[inst.operands[0]].block]
                                             .instructions[defs[inst.operands[0]].index];
                        if (di.op == il::Op::PropGet && di.keyIndex < closedKeyTargets.size() &&
                            !closedKeyTargets[di.keyIndex].empty()) {
                            for (uint32_t tgt : closedKeyTargets[di.keyIndex]) {
                                effects[f].directCallees.push_back(tgt);
                            }
                        } else {
                            effects[f].hasUnresolvedCalls = true;
                        }
                    } else {
                        effects[f].hasUnresolvedCalls = true;
                    }
                } else if (inst.op == il::Op::Construct || inst.op == il::Op::SuperCall) {
                    const uint32_t callee = findCalleeIndex(inst, fn, defs, module);
                    if (callee != UINT32_MAX && callee < module.functions.size()) {
                        if (inst.op == il::Op::Construct && callee < ctorCache.size()) {
                            if (!ctorCache[callee].has_value()) {
                                ctorCache[callee] = analyzeConstructor(module.functions[callee]);
                            }
                            if (ctorCache[callee]->isSafe) continue;
                        }
                        effects[f].directCallees.push_back(callee);
                    } else if (isBuiltinPureConstruct(inst, fn, defs, module)) {
                        // Pure built-in TypedArray/Array/Error allocation
                    } else {
                        effects[f].hasUnresolvedCalls = true;
                    }
                } else if (inst.op == il::Op::CreateObject || inst.op == il::Op::CreateArray ||
                           inst.op == il::Op::Throw) {
                    // Pure or aborts execution without mutating existing properties
                } else if (isImpureForLoopPropHoist(inst)) {
                    effects[f].hasUnresolvedCalls = true;
                }
            }
        }
        auto& keys = effects[f].writtenKeys;
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
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
                const auto& theirs = effects[callee].writtenKeys;
                auto& mine = effects[f].writtenKeys;
                if (theirs.empty() || &theirs == &mine) continue;
                std::vector<uint32_t> merged;
                merged.reserve(mine.size() + theirs.size());
                std::set_union(mine.begin(), mine.end(), theirs.begin(), theirs.end(),
                               std::back_inserter(merged));
                if (merged.size() != mine.size()) {
                    mine = std::move(merged);
                    changed = true;
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
                       uint32_t lengthKeyIndex,
                       const DirectMethodTable* directMethods,
                       bool protoPatchFree,
                       const std::vector<uint8_t>* isMutatedMethodKey,
                       const std::vector<std::vector<uint32_t>>* closedKeyTargets) {
    auto checkCalleeEffects = [&](uint32_t callee) -> bool {
        if (callee >= funcEffects.size()) return true;
        const auto& eff = funcEffects[callee];
        if (eff.hasUnresolvedCalls) return true;
        if (keyIndex == lengthKeyIndex && eff.mutatesLength) {
            if (!isNonEscapingLocalObject(recv, fn, defs)) return true;
        }
        if (eff.writes(keyIndex)) {
            for (il::ValueId op : inst.operands) {
                if (op == recv) return true;
            }
            if (!isNonEscapingLocalObject(recv, fn, defs)) return true;
        }
        return false;
    };

    switch (inst.op) {
        case il::Op::PropSet: {
            if (inst.operands.empty()) return true;
            const bool isAcc = inst.keyIndex < isAccessorKey.size() &&
                               isAccessorKey[inst.keyIndex] &&
                               !(protoPatchFree &&
                                 inst.staticSlot != il::Instruction::kNoStaticSlot);
            if (isAcc) return true;
            if (keyIndex == lengthKeyIndex &&
                inst.keyIndex < module.keyConstants.size() &&
                isArrayIndexKey(module.keyConstants[inst.keyIndex])) {
                return !isFreshLocalObject(inst.operands[0], fn, defs, inLoop);
            }
            if (inst.keyIndex != keyIndex) return false;
            return !isFreshLocalObject(inst.operands[0], fn, defs, inLoop);
        }

        case il::Op::ElemSet:
        case il::Op::ElemSetTyped: {
            if (inst.operands.empty()) return true;
            if (keyIndex != lengthKeyIndex) return false;
            return !isFreshLocalObject(inst.operands[0], fn, defs, inLoop);
        }

        case il::Op::Construct:
        case il::Op::SuperCall: {
            const uint32_t calleeIdx = findCalleeIndex(inst, fn, defs, module);
            if (calleeIdx != UINT32_MAX && calleeIdx < ctorCache.size()) {
                if (inst.op == il::Op::Construct) {
                    if (!ctorCache[calleeIdx].has_value()) {
                        ctorCache[calleeIdx] = analyzeConstructor(module.functions[calleeIdx]);
                    }
                    if (ctorCache[calleeIdx]->isSafe) return false;
                }
                return checkCalleeEffects(calleeIdx);
            }
            if (isBuiltinPureConstruct(inst, fn, defs, module)) return false;
            return true;
        }

        case il::Op::CreateObject:
        case il::Op::CreateArray:
        case il::Op::Throw:
            return false;

        case il::Op::MethodCall: {
            if (inst.operands.empty()) return true;
            const il::ValueId target = inst.operands[0];
            if (isMathObject(target, fn, defs, module)) return false;
            if (isMutatedMethodKey &&
                isSealedDirectMethodCall(inst, module, directMethods, protoPatchFree,
                                         *isMutatedMethodKey)) {
                return checkCalleeEffects(inst.directTarget);
            }
            if (protoPatchFree && closedKeyTargets &&
                inst.keyIndex < closedKeyTargets->size() &&
                !(*closedKeyTargets)[inst.keyIndex].empty()) {
                for (uint32_t tgt : (*closedKeyTargets)[inst.keyIndex]) {
                    if (checkCalleeEffects(tgt)) return true;
                }
                return false;
            }
            const uint32_t methodKey = inst.keyIndex;
            const std::string& methodName =
                (methodKey < module.keyConstants.size()) ? module.keyConstants[methodKey] : "";

            if (isArrayMutatingMethod(methodName)) {
                if (keyIndex != lengthKeyIndex) return false;
                if (target == recv) return true;
                if (isFreshLocalObject(target, fn, defs, inLoop)) return false;
                if (isFreshLocalObject(target, fn, defs, /*inLoop=*/{})) {
                    if (!isFreshLocalObject(recv, fn, defs, /*inLoop=*/{}) || target != recv) {
                        return false;
                    }
                }
                return true;
            }
            return !isArrayReadOnlyMethod(methodName);
        }

        case il::Op::Call:
        case il::Op::DynamicCall: {
            const uint32_t callee = findCalleeIndex(inst, fn, defs, module);
            if (callee != UINT32_MAX) return checkCalleeEffects(callee);
            if (isMathPropCall(inst, fn, defs, module)) return false;
            if (protoPatchFree && closedKeyTargets && !inst.operands.empty() &&
                inst.operands[0] < defs.size() && defs[inst.operands[0]].block != il::kNoBlock &&
                defs[inst.operands[0]].index <
                    fn.blocks[defs[inst.operands[0]].block].instructions.size()) {
                const auto& di = fn.blocks[defs[inst.operands[0]].block]
                                     .instructions[defs[inst.operands[0]].index];
                if (di.op == il::Op::PropGet && di.keyIndex < closedKeyTargets->size() &&
                    !(*closedKeyTargets)[di.keyIndex].empty()) {
                    for (uint32_t tgt : (*closedKeyTargets)[di.keyIndex]) {
                        if (checkCalleeEffects(tgt)) return true;
                    }
                    return false;
                }
            }
            return true;
        }

        // A read of an accessor key is a call to the getter.
        case il::Op::PropGet:
            if (protoPatchFree && inst.staticSlot != il::Instruction::kNoStaticSlot) {
                return false;
            }
            return inst.keyIndex < isAccessorKey.size() && isAccessorKey[inst.keyIndex];
        case il::Op::SuperGet:
            return inst.keyIndex < isAccessorKey.size() && isAccessorKey[inst.keyIndex];

        // Everything without a narrower answer above: the one predicate, so a
        // loop and a straight-line run agree about what can write a property.
        default:
            return isImpureForLoopPropHoist(inst);
    }
}

}  // namespace bronze::lower
