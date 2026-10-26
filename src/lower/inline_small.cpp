#include "lower/inline_small.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "il/verifier.h"
#include "lower/guard_region_cfg.h"
#include "lower/loop_prop_analysis.h"
#include "support/diagnostics.h"

namespace bronze::lower {

namespace {

constexpr uint32_t kNoKey = UINT32_MAX;

enum class KnownBool : uint8_t { Unknown, False, True };
enum class KnownTag : uint8_t { Unknown, Undefined, BoxedPrim, BoxedF64 };

bool isDynamicArith(const il::Instruction& inst) {
    return inst.type == il::Type::Dynamic &&
           (inst.op == il::Op::Add || inst.op == il::Op::Sub || inst.op == il::Op::Mul ||
            inst.op == il::Op::Div || inst.op == il::Op::Mod || inst.op == il::Op::Neg);
}

size_t countFunctionInstructions(const il::Function& fn) {
    size_t total = 0;
    for (const auto& b : fn.blocks) total += b.instructions.size();
    return total;
}

bool hasExceptionHandlers(const il::Function& fn) {
    for (const auto& b : fn.blocks) {
        if (b.handler != il::kNoBlock) return true;
    }
    return false;
}

bool functionHasLoop(const il::Function& fn) {
    if (fn.blocks.empty()) return false;
    const Cfg cfg = buildCfg(fn);
    for (il::BlockId header = 0; header < fn.blocks.size(); ++header) {
        for (il::BlockId pred : cfg.preds[header]) {
            if (cfg.dominates(header, pred)) return true;
        }
    }
    return false;
}

bool isInlinableCandidate(const il::Function& fn) {
    if (fn.needsEnv || fn.needsArguments || fn.hasRestParam ||
        fn.isGenerator || fn.isCoroutineBody() || fn.isExternal) {
        return false;
    }
    if (fn.blocks.empty() || hasExceptionHandlers(fn)) return false;
    const size_t totalInsts = countFunctionInstructions(fn);
    if (totalInsts == 0 || totalInsts > 200) return false;

    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            switch (inst.op) {
                case il::Op::EnvGet:
                case il::Op::EnvGetTdz:
                case il::Op::EnvSet:
                case il::Op::EnvInitTdz:
                case il::Op::EnvCreate:
                case il::Op::CreateFunction:
                case il::Op::CoroStart:
                case il::Op::CoroSuspend:
                case il::Op::CoroMode:
                case il::Op::SuperGet:
                case il::Op::SuperSet:
                case il::Op::SuperCall:
                case il::Op::SuperCallSpread:
                case il::Op::GetNewTarget:
                case il::Op::ImportMeta:
                case il::Op::DynamicImport:
                case il::Op::Throw:
                // A call, a construct or the iterator protocol enters another
                // function with the callee's frame below it, and that function
                // can read the stack: `new Error().stack`, console.trace, the
                // profiler. Inlined, the callee's frame is not there, so a
                // trace would name the caller at the callee's position and drop
                // a line. Only a leaf is inlined, as brass's own inliner does.
                case il::Op::Call:
                case il::Op::DynamicCall:
                case il::Op::DynamicCallSpread:
                case il::Op::MethodCall:
                case il::Op::MethodCallSpread:
                case il::Op::Construct:
                case il::Op::ConstructSpread:
                case il::Op::IterOpen:
                case il::Op::AsyncIterOpen:
                case il::Op::AsyncIterNext:
                case il::Op::AsyncIterClose:
                case il::Op::IterStep:
                case il::Op::IterClose:
                case il::Op::IterRest:
                case il::Op::IterDelegate:
                case il::Op::ArraySpread:
                    return false;
                default:
                    break;
            }
        }
    }
    return true;
}

void replaceAllValueUsesInFunction(il::Function& fn, il::ValueId from, il::ValueId to) {
    if (from == to || from == il::kNoValue || to == il::kNoValue) return;
    for (auto& block : fn.blocks) {
        for (auto& inst : block.instructions) {
            for (auto& op : inst.operands) if (op == from) op = to;
            for (auto& arg : inst.target.args) if (arg == from) arg = to;
            for (auto& arg : inst.elseTarget.args) if (arg == from) arg = to;
        }
    }
}

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

bool tryInlineCall(const il::Instruction& callInst,
                   const il::Function& callee,
                   il::Function& caller,
                   const std::vector<DefSite>& callerDefs,
                   std::vector<std::optional<SafeConstructorInfo>>& ctorCache,
                   const std::vector<il::ValueId>& callerUnboxedF64,
                   const std::vector<il::ValueId>& callerBoxedF64,
                   const std::vector<KnownBool>& callerKnownBool,
                   const std::vector<KnownTag>& callerKnownTag,
                   il::Module& module,
                   std::vector<il::Instruction>& outInsts,
                   il::ValueId& outReturnVal) {
    const size_t paramCount = callee.params.size();
    if (callInst.op == il::Op::Call) {
        if (callee.needsThis) return false;
    } else if (callInst.op == il::Op::MethodCall) {
        if (callInst.operands.empty()) return false;
    } else {
        return false;
    }

    std::vector<il::ValueId> valMap(callee.valueCount, il::kNoValue);
    std::vector<il::ValueId> unboxedF64(callee.valueCount, il::kNoValue);
    std::vector<il::ValueId> boxedF64(callee.valueCount, il::kNoValue);
    std::vector<KnownBool> knownBool(callee.valueCount, KnownBool::Unknown);
    std::vector<KnownTag> knownTag(callee.valueCount, KnownTag::Unknown);

    il::ValueId nextVal = caller.valueCount;
    il::ValueId cachedUndef = il::kNoValue;
    auto emitUndef = [&]() -> il::ValueId {
        if (cachedUndef != il::kNoValue) return cachedUndef;
        il::Instruction u;
        u.op = il::Op::ConstUndefined;
        u.type = il::Type::Dynamic;
        u.result = nextVal++;
        outInsts.push_back(u);
        cachedUndef = u.result;
        return cachedUndef;
    };

    auto bindParam = [&](il::ValueId pId, std::optional<il::ValueId> callerArg) -> bool {
        if (pId >= callee.valueCount) return false;
        if (callerArg.has_value()) {
            const il::ValueId arg = *callerArg;
            valMap[pId] = arg;
            if (arg < callerUnboxedF64.size() && callerUnboxedF64[arg] != il::kNoValue) {
                unboxedF64[pId] = callerUnboxedF64[arg];
            }
            if (arg < callerBoxedF64.size() && callerBoxedF64[arg] != il::kNoValue) {
                boxedF64[pId] = callerBoxedF64[arg];
            }
            if (arg < callerKnownBool.size()) {
                knownBool[pId] = callerKnownBool[arg];
            }
            if (arg < callerKnownTag.size() && callerKnownTag[arg] != KnownTag::Unknown) {
                knownTag[pId] = callerKnownTag[arg];
            } else if (isProvablyNonNullish(arg, caller, callerDefs, module, ctorCache)) {
                knownTag[pId] = KnownTag::BoxedPrim;
            }
        } else {
            valMap[pId] = emitUndef();
            knownTag[pId] = KnownTag::Undefined;
        }
        return true;
    };

    if (callInst.op == il::Op::Call) {
        const size_t argCount = callInst.operands.size();
        for (size_t i = 0; i < paramCount; ++i) {
            if (callee.params[i].type != il::Type::Dynamic && i >= argCount) return false;
            std::optional<il::ValueId> arg;
            if (i < argCount) arg = callInst.operands[i];
            if (!bindParam(static_cast<il::ValueId>(i), arg)) return false;
        }
    } else {
        if (callee.needsThis) {
            if (paramCount == 0) return false;
            if (!bindParam(0, callInst.operands[0])) return false;
            const size_t argCount = callInst.operands.size() - 1;
            for (size_t i = 1; i < paramCount; ++i) {
                if (callee.params[i].type != il::Type::Dynamic && (i - 1) >= argCount) {
                    return false;
                }
                std::optional<il::ValueId> arg;
                if ((i - 1) < argCount) arg = callInst.operands[i];
                if (!bindParam(static_cast<il::ValueId>(i), arg)) return false;
            }
        } else {
            const size_t argCount = callInst.operands.size() - 1;
            for (size_t i = 0; i < paramCount; ++i) {
                if (callee.params[i].type != il::Type::Dynamic && i >= argCount) return false;
                std::optional<il::ValueId> arg;
                if (i < argCount) arg = callInst.operands[1 + i];
                if (!bindParam(static_cast<il::ValueId>(i), arg)) return false;
            }
        }
    }

    uint32_t curBlock = 0;
    uint32_t steps = 0;
    uint32_t remainingDynamicArith = 0;
    uint32_t remainingCheckedUnbox = 0;

    while (steps++ < 12) {
        if (curBlock >= callee.blocks.size()) return false;
        const auto& block = callee.blocks[curBlock];
        if (block.instructions.empty()) return false;

        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const auto& inst = block.instructions[i];
            const bool isLast = (i + 1 == block.instructions.size());

            if (il::isTerminator(inst.op)) {
                if (!isLast) return false;
                if (inst.op == il::Op::Ret) {
                    if (remainingDynamicArith > 0 || remainingCheckedUnbox > 2) {
                        return false;
                    }
                    if (outInsts.size() > 100) return false;
                    il::ValueId retVal = il::kNoValue;
                    if (inst.operands.empty() || inst.operands[0] == il::kNoValue) {
                        retVal = emitUndef();
                    } else {
                        if (inst.operands[0] >= valMap.size() ||
                            valMap[inst.operands[0]] == il::kNoValue) {
                            return false;
                        }
                        retVal = valMap[inst.operands[0]];
                    }
                    outReturnVal = retVal;
                    caller.valueCount = nextVal;
                    return true;
                }
                auto advanceToTarget = [&](const il::BlockTarget& bt) -> bool {
                    const uint32_t nextB = bt.block;
                    if (nextB >= callee.blocks.size()) return false;
                    const auto& targetParams = callee.blocks[nextB].params;
                    const auto& args = bt.args;
                    if (args.size() != targetParams.size()) return false;
                    std::vector<il::ValueId> nextValMap(targetParams.size());
                    std::vector<il::ValueId> nextUnboxed(targetParams.size());
                    std::vector<il::ValueId> nextBoxed(targetParams.size());
                    std::vector<KnownBool> nextBool(targetParams.size());
                    std::vector<KnownTag> nextTag(targetParams.size());
                    for (size_t k = 0; k < args.size(); ++k) {
                        const il::ValueId a = args[k];
                        if (a >= valMap.size() || valMap[a] == il::kNoValue) return false;
                        nextValMap[k] = valMap[a];
                        nextUnboxed[k] = unboxedF64[a];
                        nextBoxed[k] = boxedF64[a];
                        nextBool[k] = knownBool[a];
                        nextTag[k] = knownTag[a];
                    }
                    for (size_t k = 0; k < targetParams.size(); ++k) {
                        const il::ValueId p = targetParams[k].id;
                        if (p >= valMap.size()) return false;
                        valMap[p] = nextValMap[k];
                        unboxedF64[p] = nextUnboxed[k];
                        boxedF64[p] = nextBoxed[k];
                        knownBool[p] = nextBool[k];
                        knownTag[p] = nextTag[k];
                    }
                    curBlock = nextB;
                    return true;
                };
                if (inst.op == il::Op::Jump) {
                    if (!advanceToTarget(inst.target)) return false;
                    break;
                }
                if (inst.op == il::Op::Branch) {
                    if (inst.operands.empty()) return false;
                    const il::ValueId cond = inst.operands[0];
                    if (cond >= knownBool.size() || knownBool[cond] == KnownBool::Unknown) {
                        return false;
                    }
                    const il::BlockTarget& chosen =
                        (knownBool[cond] == KnownBool::True) ? inst.target : inst.elseTarget;
                    if (!advanceToTarget(chosen)) return false;
                    break;
                }
                return false;
            }

            if (isLast) return false;

            if (inst.op == il::Op::Unbox && inst.type == il::Type::F64 &&
                !inst.operands.empty()) {
                const il::ValueId src = inst.operands[0];
                if (src < unboxedF64.size() && unboxedF64[src] != il::kNoValue) {
                    if (inst.result >= valMap.size()) return false;
                    valMap[inst.result] = unboxedF64[src];
                    if (src < valMap.size()) boxedF64[inst.result] = valMap[src];
                    continue;
                }
            }

            if (inst.op == il::Op::Box && inst.boxType == il::Type::F64 &&
                !inst.operands.empty()) {
                const il::ValueId src = inst.operands[0];
                if (src < boxedF64.size() && boxedF64[src] != il::kNoValue) {
                    if (inst.result >= valMap.size()) return false;
                    valMap[inst.result] = boxedF64[src];
                    if (src < valMap.size()) unboxedF64[inst.result] = valMap[src];
                    knownTag[inst.result] = KnownTag::BoxedF64;
                    continue;
                }
            }

            if ((inst.op == il::Op::StrictEq || inst.op == il::Op::CmpEq) &&
                inst.operands.size() == 2 && inst.result != il::kNoValue &&
                inst.result < valMap.size()) {
                const il::ValueId lhs = inst.operands[0];
                const il::ValueId rhs = inst.operands[1];
                std::optional<bool> folded;
                if (inst.op == il::Op::StrictEq && lhs < knownTag.size() && rhs < knownTag.size()) {
                    const KnownTag t0 = knownTag[lhs];
                    const KnownTag t1 = knownTag[rhs];
                    const auto isNonUndef = [](KnownTag t) {
                        return t == KnownTag::BoxedPrim || t == KnownTag::BoxedF64;
                    };
                    if ((t0 == KnownTag::Undefined && isNonUndef(t1)) ||
                        (isNonUndef(t0) && t1 == KnownTag::Undefined)) {
                        folded = false;
                    } else if (t0 == KnownTag::Undefined && t1 == KnownTag::Undefined) {
                        folded = true;
                    }
                }
                if (!folded.has_value() && lhs < knownBool.size() && rhs < knownBool.size() &&
                    knownBool[lhs] != KnownBool::Unknown &&
                    knownBool[rhs] != KnownBool::Unknown) {
                    folded = (knownBool[lhs] == knownBool[rhs]);
                }
                if (folded.has_value()) {
                    il::Instruction cInst;
                    cInst.op = il::Op::ConstBool;
                    cInst.type = il::Type::Bool;
                    cInst.result = nextVal++;
                    cInst.immI32 = *folded ? 1 : 0;
                    valMap[inst.result] = cInst.result;
                    knownBool[inst.result] = *folded ? KnownBool::True : KnownBool::False;
                    outInsts.push_back(cInst);
                    continue;
                }
            }

            il::Instruction cloned = inst;
            for (auto& op : cloned.operands) {
                if (op >= valMap.size() || valMap[op] == il::kNoValue) return false;
                op = valMap[op];
            }
            if (inst.result != il::kNoValue) {
                if (inst.result >= valMap.size()) return false;
                cloned.result = nextVal++;
                valMap[inst.result] = cloned.result;
            }

            if ((cloned.op == il::Op::PropGet || cloned.op == il::Op::PropSet) &&
                module.censusOutPath.empty()) {
                cloned.icIndex = module.icSiteCount++;
            }

            if (inst.result != il::kNoValue && inst.result < callee.valueCount) {
                if (inst.op == il::Op::ConstBool) {
                    knownBool[inst.result] =
                        (inst.immI32 != 0) ? KnownBool::True : KnownBool::False;
                } else if (inst.op == il::Op::ConstUndefined) {
                    knownTag[inst.result] = KnownTag::Undefined;
                } else if (inst.op == il::Op::CreateObject || inst.op == il::Op::CreateArray ||
                           inst.op == il::Op::Construct) {
                    knownTag[inst.result] = KnownTag::BoxedPrim;
                } else if (inst.op == il::Op::IsNumber && !inst.operands.empty()) {
                    const il::ValueId src = inst.operands[0];
                    if (src < unboxedF64.size() &&
                        (unboxedF64[src] != il::kNoValue ||
                         knownTag[src] == KnownTag::BoxedF64)) {
                        knownBool[inst.result] = KnownBool::True;
                    } else if (src < knownTag.size() &&
                               knownTag[src] == KnownTag::Undefined) {
                        knownBool[inst.result] = KnownBool::False;
                    }
                } else if (inst.op == il::Op::Box) {
                    if (inst.boxType == il::Type::F64 && !inst.operands.empty()) {
                        const il::ValueId src = inst.operands[0];
                        if (src < valMap.size()) {
                            unboxedF64[inst.result] = valMap[src];
                            boxedF64[src] = cloned.result;
                        }
                        knownTag[inst.result] = KnownTag::BoxedF64;
                    } else if (inst.boxType == il::Type::Bool && !inst.operands.empty()) {
                        const il::ValueId src = inst.operands[0];
                        if (src < knownBool.size()) knownBool[inst.result] = knownBool[src];
                        knownTag[inst.result] = KnownTag::BoxedPrim;
                    } else {
                        knownTag[inst.result] = KnownTag::BoxedPrim;
                    }
                } else if (inst.op == il::Op::Unbox && !inst.operands.empty()) {
                    const il::ValueId src = inst.operands[0];
                    if (inst.type == il::Type::F64 && src < valMap.size()) {
                        boxedF64[inst.result] = valMap[src];
                        unboxedF64[src] = cloned.result;
                    } else if (inst.type == il::Type::Bool && src < knownBool.size()) {
                        knownBool[inst.result] = knownBool[src];
                    }
                }
            }

            if (isDynamicArith(cloned)) {
                ++remainingDynamicArith;
            }
            if (cloned.op == il::Op::Unbox && cloned.type == il::Type::F64 &&
                !cloned.rawUnbox && !cloned.nullishUnbox) {
                ++remainingCheckedUnbox;
            }

            outInsts.push_back(std::move(cloned));
            if (outInsts.size() > 100) return false;
        }
    }
    return false;
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

    for (size_t fnIdx = 0; fnIdx < numFns; ++fnIdx) {
        const auto& fn = module.functions[fnIdx];
        const il::ValueId thisParamId = fn.needsEnv ? 1u : 0u;
        std::vector<int32_t> funcValCallee(fn.valueCount, -1);
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

        for (const auto& fn : module.functions) {
            std::vector<int32_t> funcValCallee(fn.valueCount, -1);
            for (const auto& block : fn.blocks) {
                for (const auto& inst : block.instructions) {
                    if (inst.result != il::kNoValue && inst.result < fn.valueCount &&
                        (inst.op == il::Op::CreateFunction || inst.op == il::Op::FunctionRef) &&
                        inst.calleeIndex < numFns) {
                        funcValCallee[inst.result] = static_cast<int32_t>(inst.calleeIndex);
                    }
                }
            }
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
            if (!valid[fw.targetKey]) break;
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

bool inlineSmallFunctions(il::Module& module, const DirectMethodTable* directMethods) {
    if (std::getenv("BRONZE_NO_INLINE_SMALL") != nullptr) {
        return false;
    }

    std::vector<uint8_t> fnHasLoop(module.functions.size(), 0);
    bool anyLoop = false;
    for (size_t i = 0; i < module.functions.size(); ++i) {
        if (functionHasLoop(module.functions[i])) {
            fnHasLoop[i] = 1;
            anyLoop = true;
        }
    }
    if (!anyLoop) return false;

    for (size_t i = 0; i < module.functions.size(); ++i) {
        if (!fnHasLoop[i]) continue;
        for (const auto& blk : module.functions[i].blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::Call && inst.calleeIndex < fnHasLoop.size()) {
                    fnHasLoop[inst.calleeIndex] |= 2;
                } else if (inst.directTarget != il::Instruction::kNoDirectTarget &&
                           inst.directTarget < fnHasLoop.size()) {
                    fnHasLoop[inst.directTarget] |= 2;
                }
            }
        }
    }

    const bool protoPatchFree = isModulePrototypePatchFree(module);
    const std::vector<uint8_t> isMutatedMethodKey =
        protoPatchFree ? computeMutatedMethodKeyMask(module) : std::vector<uint8_t>{};

    std::vector<uint8_t> inlinable(module.functions.size(), 0);
    for (size_t i = 0; i < module.functions.size(); ++i) {
        if (isInlinableCandidate(module.functions[i])) {
            inlinable[i] = 1;
        }
    }

    std::vector<std::optional<SafeConstructorInfo>> ctorCache(module.functions.size());
    bool anyChanged = false;

    for (size_t callerIdx = 0; callerIdx < module.functions.size(); ++callerIdx) {
        if (!fnHasLoop[callerIdx]) continue;
        il::Function& caller = module.functions[callerIdx];
        if (hasExceptionHandlers(caller)) continue;

        for (int pass = 0; pass < 3; ++pass) {
            if (countFunctionInstructions(caller) > 800) break;

            const il::Function callerBackup = caller;
            const uint32_t icSiteBackup = module.icSiteCount;
            bool callerChanged = false;

            std::vector<DefSite> callerDefs = computeDefSites(caller);
            std::vector<il::ValueId> callerUnboxedF64(caller.valueCount, il::kNoValue);
            std::vector<il::ValueId> callerBoxedF64(caller.valueCount, il::kNoValue);
            std::vector<KnownBool> callerKnownBool(caller.valueCount, KnownBool::Unknown);
            std::vector<KnownTag> callerKnownTag(caller.valueCount, KnownTag::Unknown);

            for (const auto& block : caller.blocks) {
                for (const auto& inst : block.instructions) {
                    if (inst.result == il::kNoValue || inst.result >= caller.valueCount) continue;
                    if (inst.op == il::Op::ConstUndefined) {
                        callerKnownTag[inst.result] = KnownTag::Undefined;
                    } else if (inst.op == il::Op::ConstBool) {
                        callerKnownBool[inst.result] =
                            (inst.immI32 != 0) ? KnownBool::True : KnownBool::False;
                    } else if (inst.op == il::Op::CreateObject ||
                               inst.op == il::Op::CreateArray ||
                               inst.op == il::Op::Construct) {
                        callerKnownTag[inst.result] = KnownTag::BoxedPrim;
                    } else if (inst.op == il::Op::Box) {
                        if (inst.boxType == il::Type::F64 && !inst.operands.empty()) {
                            const il::ValueId src = inst.operands[0];
                            if (src < caller.valueCount) {
                                callerUnboxedF64[inst.result] = src;
                                callerBoxedF64[src] = inst.result;
                            }
                            callerKnownTag[inst.result] = KnownTag::BoxedF64;
                        } else if (inst.boxType == il::Type::Bool && !inst.operands.empty()) {
                            const il::ValueId src = inst.operands[0];
                            if (src < caller.valueCount) {
                                callerKnownBool[inst.result] = callerKnownBool[src];
                            }
                            callerKnownTag[inst.result] = KnownTag::BoxedPrim;
                        } else {
                            callerKnownTag[inst.result] = KnownTag::BoxedPrim;
                        }
                    } else if (inst.op == il::Op::Unbox && inst.type == il::Type::F64 &&
                               !inst.operands.empty()) {
                        const il::ValueId src = inst.operands[0];
                        if (src < caller.valueCount) {
                            callerBoxedF64[inst.result] = src;
                            callerUnboxedF64[src] = inst.result;
                        }
                    }
                }
            }

            std::vector<std::pair<il::ValueId, il::ValueId>> replacements;

            for (size_t bIdx = 0; bIdx < caller.blocks.size(); ++bIdx) {
                auto& block = caller.blocks[bIdx];
                std::vector<il::Instruction> newInsts;
                newInsts.reserve(block.instructions.size());
                bool blockChanged = false;

                for (size_t i = 0; i < block.instructions.size(); ++i) {
                    const auto& inst = block.instructions[i];
                    uint32_t calleeIdx = UINT32_MAX;

                    if (inst.op == il::Op::Call &&
                        inst.callEnvHops == il::Instruction::kNoEnvHops) {
                        calleeIdx = findCalleeIndex(inst, caller, callerDefs, module);
                    } else if (isSealedDirectMethodCall(inst, module, directMethods,
                                                        protoPatchFree, isMutatedMethodKey)) {
                        calleeIdx = inst.directTarget;
                    }

                    if (calleeIdx != UINT32_MAX && calleeIdx != callerIdx &&
                        calleeIdx < module.functions.size() && inlinable[calleeIdx]) {
                        std::vector<il::Instruction> inlinedInsts;
                        il::ValueId retVal = il::kNoValue;
                        if (tryInlineCall(inst, module.functions[calleeIdx], caller,
                                          callerDefs, ctorCache, callerUnboxedF64,
                                          callerBoxedF64, callerKnownBool, callerKnownTag,
                                          module, inlinedInsts, retVal)) {
                            for (auto& inl : inlinedInsts) {
                                newInsts.push_back(std::move(inl));
                            }
                            if (inst.result != il::kNoValue && retVal != il::kNoValue) {
                                replacements.emplace_back(inst.result, retVal);
                            }
                            blockChanged = true;
                            callerChanged = true;
                            continue;
                        }
                    }

                    newInsts.push_back(inst);
                }

                if (blockChanged) {
                    block.instructions = std::move(newInsts);
                }
            }

            if (!callerChanged) break;

            for (const auto& [from, to] : replacements) {
                replaceAllValueUsesInFunction(caller, from, to);
            }

            DiagnosticSink diags;
            if (!il::verifyFunction(caller, diags)) {
                caller = callerBackup;
                module.icSiteCount = icSiteBackup;
                break;
            }
            anyChanged = true;
        }
    }

    return anyChanged;
}

}  // namespace bronze::lower
