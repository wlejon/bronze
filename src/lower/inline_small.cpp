#include "lower/inline_small.h"

#include <cstdlib>
#include <functional>
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

// The largest body inlined into a caller with no loop (inlineSmallFunctions).

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

bool tryInlineCall(const il::Instruction& callInst,
                   uint32_t calleeIdx,
                   const il::Function& callee,
                   il::Function& caller,
                   const std::vector<DefSite>& callerDefs,
                   std::vector<std::optional<SafeConstructorInfo>>& ctorCache,
                   const std::vector<il::ValueId>& callerUnboxedF64,
                   const std::vector<il::ValueId>& callerBoxedF64,
                   const std::function<bool(il::ValueId)>& available,
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

    // The frames the copied instructions name (il.h `inlineFrames`), so a
    // stack trace taken in the copy still shows the callee's frame at the
    // call's position: the callee's own frame first, then the frames of
    // what was inlined into it, re-parented under it. They join the
    // caller's table only when the copy is taken.
    const uint32_t frameBase = static_cast<uint32_t>(caller.inlineFrames.size());
    const uint32_t ownFrame = frameBase + 1;
    std::vector<il::Function::InlineFrame> newFrames;
    newFrames.reserve(1 + callee.inlineFrames.size());
    newFrames.push_back(il::Function::InlineFrame{calleeIdx, callInst.span, callInst.inlineFrame});
    for (const auto& f : callee.inlineFrames) {
        newFrames.push_back(il::Function::InlineFrame{f.callee, f.callSpan, f.parent == 0 ? ownFrame : ownFrame + f.parent});
    }
    const auto frameOf = [&](uint32_t calleeFrame) {
        return calleeFrame == 0 || calleeFrame > callee.inlineFrames.size() ? ownFrame : ownFrame + calleeFrame;
    };

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
            if (arg < callerUnboxedF64.size() && callerUnboxedF64[arg] != il::kNoValue &&
                available(callerUnboxedF64[arg])) {
                unboxedF64[pId] = callerUnboxedF64[arg];
            }
            if (arg < callerBoxedF64.size() && callerBoxedF64[arg] != il::kNoValue &&
                available(callerBoxedF64[arg])) {
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

    const size_t opOffset = (callInst.op != il::Op::Call && !callee.needsThis) ? 1 : 0;
    if (callInst.op != il::Op::Call && callee.needsThis && paramCount == 0) return false;
    for (size_t i = 0; i < paramCount; ++i) {
        const size_t opIdx = i + opOffset;
        if (callee.params[i].type != il::Type::Dynamic && opIdx >= callInst.operands.size()) {
            return false;
        }
        std::optional<il::ValueId> arg;
        if (opIdx < callInst.operands.size()) arg = callInst.operands[opIdx];
        if (!bindParam(static_cast<il::ValueId>(i), arg)) return false;
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
                    caller.inlineFrames.insert(caller.inlineFrames.end(), newFrames.begin(), newFrames.end());
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
            cloned.inlineFrame = frameOf(inst.inlineFrame);
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

bool inlineSmallFunctions(il::Module& module, const DirectMethodTable* directMethods) {
    std::vector<uint8_t> fnHasLoop(module.functions.size(), 0);
    for (size_t i = 0; i < module.functions.size(); ++i) {
        if (functionHasLoop(module.functions[i])) fnHasLoop[i] = 1;
    }

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
            const Cfg callerCfg = buildCfg(caller);
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
                        // A box or unbox of an argument stands in for the
                        // callee's own only where it dominates the call.
                        const auto available = [&](il::ValueId v) {
                            if (v < caller.params.size()) return true;
                            if (v >= callerDefs.size() || callerDefs[v].block == il::kNoBlock) return false;
                            const DefSite& d = callerDefs[v];
                            if (d.block == bIdx) return d.index == UINT32_MAX || d.index < i;
                            return callerCfg.dominates(d.block, static_cast<il::BlockId>(bIdx));
                        };
                        if (tryInlineCall(inst, calleeIdx, module.functions[calleeIdx], caller,
                                          callerDefs, ctorCache, callerUnboxedF64,
                                          callerBoxedF64, available, callerKnownBool, callerKnownTag,
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
