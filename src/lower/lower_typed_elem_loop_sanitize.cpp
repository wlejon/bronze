// Lowered-IL loop verification pass that strips `kElemFlagProvenInBounds`
// whenever any instruction in the bounding natural loop can invoke user JS or
// detach/resize an ArrayBuffer.

#include <functional>
#include <string>
#include <vector>

#include "lower/guard_region_cfg.h"
#include "lower/inline_small.h"
#include "lower/loop_prop_analysis.h"
#include "lower/lowerer.h"

namespace bronze::lower {

namespace {

il::ValueId unwrapBoxValue(il::ValueId val, const il::Function& fn,
                           const std::vector<DefSite>& defs) {
    if (val >= defs.size() || defs[val].block == il::kNoBlock) return val;
    const auto& blk = fn.blocks[defs[val].block];
    if (defs[val].index >= blk.instructions.size()) return val;
    const auto& inst = blk.instructions[defs[val].index];
    if (inst.op == il::Op::Box && !inst.operands.empty()) {
        return inst.operands[0];
    }
    return val;
}

bool sameReceiverValue(il::ValueId a, il::ValueId b, const il::Function& fn,
                       const std::vector<DefSite>& defs) {
    return unwrapBoxValue(a, fn, defs) == unwrapBoxValue(b, fn, defs);
}

bool isKnownTypedReceiver(il::ValueId a, il::ValueId recv, const il::Function& fn,
                          const std::vector<DefSite>& defs) {
    if (sameReceiverValue(a, recv, fn, defs)) return true;
    const il::ValueId root = unwrapBoxValue(a, fn, defs);
    for (const il::Block& blk : fn.blocks) {
        for (const il::Instruction& inst : blk.instructions) {
            if ((inst.op == il::Op::ElemGetTyped || inst.op == il::Op::ElemSetTyped) &&
                !inst.operands.empty() && unwrapBoxValue(inst.operands[0], fn, defs) == root) {
                return true;
            }
        }
    }
    return false;
}

bool isProvablyPrimitiveOrLengthRead(il::ValueId val, il::ValueId recv,
                                     uint32_t lengthKeyIndex, const il::Function& fn,
                                     const std::vector<DefSite>& defs,
                                     const std::vector<uint8_t>& isAccessorKey,
                                     bool protoPatchFree, int depth = 4) {
    if (depth <= 0 || val >= defs.size()) return false;
    if (defs[val].block == il::kNoBlock) {
        return val < fn.params.size() &&
               (fn.params[val].type == il::Type::F64 || fn.params[val].type == il::Type::I32 ||
                fn.params[val].type == il::Type::Bool || fn.params[val].type == il::Type::Str);
    }
    const auto& blk = fn.blocks[defs[val].block];
    if (defs[val].index >= blk.instructions.size()) {
        for (size_t pIdx = 0; pIdx < blk.params.size(); ++pIdx) {
            const auto& bp = blk.params[pIdx];
            if (bp.id != val) continue;
            if (bp.type == il::Type::F64 || bp.type == il::Type::I32 ||
                bp.type == il::Type::Bool || bp.type == il::Type::Str) {
                return true;
            }
            if (depth <= 1) return false;
            bool foundEdge = false;
            for (const auto& predBlk : fn.blocks) {
                if (predBlk.instructions.empty()) continue;
                const auto& term = predBlk.instructions.back();
                auto checkTarget = [&](const il::BlockTarget& t) {
                    if (t.block != defs[val].block) return true;
                    if (pIdx >= t.args.size()) return false;
                    foundEdge = true;
                    return isProvablyPrimitiveOrLengthRead(t.args[pIdx], recv, lengthKeyIndex, fn,
                                                           defs, isAccessorKey, protoPatchFree,
                                                           depth - 1);
                };
                if ((term.op == il::Op::Jump || term.op == il::Op::Branch) &&
                    !checkTarget(term.target)) {
                    return false;
                }
                if (term.op == il::Op::Branch && !checkTarget(term.elseTarget)) {
                    return false;
                }
            }
            return foundEdge;
        }
        return false;
    }

    const auto& inst = blk.instructions[defs[val].index];
    if (inst.type == il::Type::F64 || inst.type == il::Type::I32 ||
        inst.type == il::Type::Bool || inst.type == il::Type::Str) {
        return true;
    }

    switch (inst.op) {
        case il::Op::ConstF64:
        case il::Op::ConstI32:
        case il::Op::ConstBool:
        case il::Op::ConstUndefined:
        case il::Op::ConstNull:
        case il::Op::ConstBigInt:
        case il::Op::ElemGetTyped:
        case il::Op::TypeOf:
        case il::Op::ToStr:
        case il::Op::ToInt32:
        case il::Op::ToNumeric:
        case il::Op::NumericStep:
        case il::Op::CmpLt:
        case il::Op::CmpGt:
        case il::Op::CmpLe:
        case il::Op::CmpGe:
        case il::Op::CmpEq:
        case il::Op::CmpNe:
        case il::Op::StrictEq:
        case il::Op::LooseEq:
        case il::Op::RelLt:
        case il::Op::RelGt:
        case il::Op::RelLe:
        case il::Op::RelGe:
        case il::Op::IsNullish:
        case il::Op::IsNumber:
        case il::Op::IsDenseArray:
        case il::Op::NumTruthy:
        case il::Op::MathUnary:
        case il::Op::MathImul:
        case il::Op::Add:
        case il::Op::Sub:
        case il::Op::Mul:
        case il::Op::Div:
        case il::Op::Mod:
        case il::Op::Pow:
        case il::Op::Neg:
        case il::Op::BitAnd:
        case il::Op::BitOr:
        case il::Op::BitXor:
        case il::Op::BitNot:
        case il::Op::Shl:
        case il::Op::Shr:
        case il::Op::UShr:
        case il::Op::ConcatEnd:
            return true;

        case il::Op::Box:
            if (inst.boxType == il::Type::F64 || inst.boxType == il::Type::I32 ||
                inst.boxType == il::Type::Bool || inst.boxType == il::Type::Str) {
                return true;
            }
            if (!inst.operands.empty()) {
                return isProvablyPrimitiveOrLengthRead(inst.operands[0], recv, lengthKeyIndex, fn,
                                                       defs, isAccessorKey, protoPatchFree,
                                                       depth - 1);
            }
            return false;

        case il::Op::PropGet:
            if (!protoPatchFree || inst.keyIndex != lengthKeyIndex) return false;
            if (lengthKeyIndex < isAccessorKey.size() && isAccessorKey[lengthKeyIndex]) {
                return false;
            }
            return recv != il::kNoValue && !inst.operands.empty() &&
                   isKnownTypedReceiver(inst.operands[0], recv, fn, defs);

        default:
            return false;
    }
}

bool isSafeInstForInBoundsLoop(const il::Instruction& inst, il::ValueId recv,
                               const il::Function& fn, const std::vector<DefSite>& defs,
                               uint32_t lengthKeyIndex,
                               const std::vector<uint8_t>& isAccessorKey,
                               bool protoPatchFree) {
    switch (inst.op) {
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
        case il::Op::CensusRecord:
        case il::Op::KeepAlive:
        case il::Op::PinGuard:
        case il::Op::EnvGet:
        case il::Op::EnvGetTdz:
        case il::Op::ModuleEnvGet:
        case il::Op::ElemGetTyped:
        case il::Op::ElemSetTyped:
        case il::Op::Jump:
        case il::Op::Branch:
        case il::Op::Ret:
            return true;

        case il::Op::MathUnary:
        case il::Op::MathImul:
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
        case il::Op::Shl:
        case il::Op::Shr:
        case il::Op::UShr:
        case il::Op::BitNot:
            return inst.type == il::Type::F64 || inst.type == il::Type::I32;

        case il::Op::ToInt32:
            return inst.boxType == il::Type::F64 || inst.boxType == il::Type::I32 ||
                   inst.boxType == il::Type::Bool;

        case il::Op::PropGet:
            if (!protoPatchFree || inst.keyIndex != lengthKeyIndex) return false;
            if (lengthKeyIndex < isAccessorKey.size() && isAccessorKey[lengthKeyIndex]) {
                return false;
            }
            return !inst.operands.empty() &&
                   isKnownTypedReceiver(inst.operands[0], recv, fn, defs);

        case il::Op::Unbox:
            if (inst.rawUnbox || inst.nullishUnbox) return true;
            if (inst.operands.empty()) return false;
            return isProvablyPrimitiveOrLengthRead(inst.operands[0], recv, lengthKeyIndex, fn,
                                                   defs, isAccessorKey, protoPatchFree);

        default:
            return false;
    }
}

}  // namespace

bool canMutateLengthViaCoercion(const il::Instruction& inst, il::ValueId recv,
                                uint32_t lengthKeyIndex, const il::Function& fn,
                                const std::vector<DefSite>& defs,
                                const std::vector<uint8_t>& isAccessorKey,
                                bool protoPatchFree) {
    auto opIsSafe = [&](il::ValueId v) {
        return isProvablyPrimitiveOrLengthRead(v, recv, lengthKeyIndex, fn, defs, isAccessorKey,
                                               protoPatchFree);
    };
    switch (inst.op) {
        case il::Op::Unbox:
            if (inst.rawUnbox || inst.nullishUnbox) return false;
            return inst.operands.empty() || !opIsSafe(inst.operands[0]);

        case il::Op::ToInt32:
            if (inst.boxType == il::Type::F64 || inst.boxType == il::Type::I32 ||
                inst.boxType == il::Type::Bool) {
                return false;
            }
            return inst.operands.empty() || !opIsSafe(inst.operands[0]);

        case il::Op::ToNumeric:
        case il::Op::NumericStep:
        case il::Op::ToStr:
        case il::Op::ConcatAppend:
            return inst.operands.empty() || !opIsSafe(inst.operands[0]);

        case il::Op::Neg:
        case il::Op::BitNot:
            if (inst.type != il::Type::Dynamic) return false;
            return inst.operands.empty() || !opIsSafe(inst.operands[0]);

        case il::Op::Add:
        case il::Op::Sub:
        case il::Op::Mul:
        case il::Op::Div:
        case il::Op::Mod:
        case il::Op::Pow:
        case il::Op::BitAnd:
        case il::Op::BitOr:
        case il::Op::BitXor:
        case il::Op::Shl:
        case il::Op::Shr:
        case il::Op::UShr:
            if (inst.type != il::Type::Dynamic) return false;
            return inst.operands.size() < 2 || !opIsSafe(inst.operands[0]) ||
                   !opIsSafe(inst.operands[1]);

        case il::Op::RelLt:
        case il::Op::RelGt:
        case il::Op::RelLe:
        case il::Op::RelGe:
        case il::Op::LooseEq:
            return inst.operands.size() < 2 || !opIsSafe(inst.operands[0]) ||
                   !opIsSafe(inst.operands[1]);

        case il::Op::ElemGet:
        case il::Op::In:
        case il::Op::InstanceOf:
        case il::Op::ObjectKeys:
        case il::Op::ForInKeys:
            return true;

        default:
            return false;
    }
}

void sanitizeLoopInBoundsProofs(il::Module& module) {
    uint32_t lengthKeyIndex = UINT32_MAX;
    std::vector<uint8_t> isAccessorKey(module.keyConstants.size(), 0);
    for (size_t k = 0; k < module.keyConstants.size(); ++k) {
        const std::string& name = module.keyConstants[k];
        if (name == "length") lengthKeyIndex = static_cast<uint32_t>(k);
        if (name == "size" || name == "flags" || name == "source" || name == "buffer" ||
            name == "byteLength" || name == "byteOffset" || name == "__proto__" ||
            name == "lastIndex") {
            isAccessorKey[k] = 1;
        }
    }
    for (const il::Function& fn : module.functions) {
        for (const il::Block& blk : fn.blocks) {
            for (const il::Instruction& inst : blk.instructions) {
                if (inst.op == il::Op::AccessorDef && inst.keyIndex < isAccessorKey.size()) {
                    isAccessorKey[inst.keyIndex] = 1;
                }
            }
        }
    }
    const bool protoPatchFree = isModulePrototypePatchFree(module);

    struct LoopDesc {
        il::BlockId header;
        std::vector<uint8_t> inLoop;
        std::vector<il::BlockId> loopBlocks;
    };

    for (il::Function& fn : module.functions) {
        bool hasProvenInBounds = false;
        for (const il::Block& blk : fn.blocks) {
            for (const il::Instruction& inst : blk.instructions) {
                if ((inst.op == il::Op::ElemGetTyped || inst.op == il::Op::ElemSetTyped) &&
                    (inst.immI32 & static_cast<int32_t>(il::kElemFlagProvenInBounds)) != 0) {
                    hasProvenInBounds = true;
                    break;
                }
            }
            if (hasProvenInBounds) break;
        }
        if (!hasProvenInBounds) continue;

        const Cfg cfg = buildCfg(fn);
        const std::vector<DefSite> defs = computeDefSites(fn);
        std::vector<LoopDesc> loops;
        for (il::BlockId header = 0; header < fn.blocks.size(); ++header) {
            std::vector<il::BlockId> backedges;
            for (il::BlockId pred : cfg.preds[header]) {
                if (cfg.dominates(header, pred)) backedges.push_back(pred);
            }
            if (backedges.empty()) continue;
            LoopDesc ld{header, std::vector<uint8_t>(fn.blocks.size(), 0), {}};
            for (il::BlockId tail : backedges) {
                for (il::BlockId lb : naturalLoop(cfg, header, tail)) {
                    if (lb < ld.inLoop.size() && !ld.inLoop[lb]) {
                        ld.inLoop[lb] = 1;
                        ld.loopBlocks.push_back(lb);
                    }
                }
            }
            loops.push_back(std::move(ld));
        }

        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            for (il::Instruction& inst : fn.blocks[b].instructions) {
                if ((inst.op != il::Op::ElemGetTyped && inst.op != il::Op::ElemSetTyped) ||
                    (inst.immI32 & static_cast<int32_t>(il::kElemFlagProvenInBounds)) == 0) {
                    continue;
                }
                bool valid = false;
                if (inst.operands.size() >= 2) {
                    const il::ValueId recv = inst.operands[0];
                    il::ValueId idxRoot = inst.operands[1];
                    if (idxRoot < defs.size() && defs[idxRoot].block != il::kNoBlock) {
                        const auto& dBlk = fn.blocks[defs[idxRoot].block];
                        if (defs[idxRoot].index < dBlk.instructions.size()) {
                            const auto& di = dBlk.instructions[defs[idxRoot].index];
                            if (di.op == il::Op::Unbox && !di.operands.empty()) {
                                idxRoot = di.operands[0];
                            }
                        }
                    }

                    std::function<bool(il::ValueId, int)> isBoundedIdxExpr =
                        [&](il::ValueId v, int depth) -> bool {
                        if (depth <= 0 || v >= defs.size()) return false;
                        if (defs[v].block == il::kNoBlock) {
                            return v < fn.params.size() &&
                                   (fn.params[v].type == il::Type::F64 ||
                                    fn.params[v].type == il::Type::I32);
                        }
                        for (const LoopDesc& anyLd : loops) {
                            if (b < anyLd.inLoop.size() && anyLd.inLoop[b]) {
                                for (const auto& bp : fn.blocks[anyLd.header].params) {
                                    if (bp.id == v) return true;
                                }
                            }
                        }
                        const auto& iBlk = fn.blocks[defs[v].block];
                        if (defs[v].index >= iBlk.instructions.size()) return false;
                        const auto& di = iBlk.instructions[defs[v].index];
                        switch (di.op) {
                            case il::Op::ConstF64:
                            case il::Op::ConstI32:
                            case il::Op::BitAnd:
                            case il::Op::UShr:
                                return true;
                            case il::Op::Add:
                            case il::Op::Sub:
                            case il::Op::Mul:
                            case il::Op::Shl:
                            case il::Op::Shr:
                            case il::Op::BitOr:
                            case il::Op::BitXor:
                            case il::Op::MathImul:
                                return di.operands.size() == 2 &&
                                       isBoundedIdxExpr(di.operands[0], depth - 1) &&
                                       isBoundedIdxExpr(di.operands[1], depth - 1);
                            case il::Op::ToInt32:
                            case il::Op::Unbox:
                                return !di.operands.empty() &&
                                       isBoundedIdxExpr(di.operands[0], depth - 1);
                            default:
                                return false;
                        }
                    };

                    for (const LoopDesc& ld : loops) {
                        if (b >= ld.inLoop.size() || !ld.inLoop[b]) continue;
                        if (!isBoundedIdxExpr(idxRoot, 6)) continue;

                        const il::ValueId recvRoot = unwrapBoxValue(recv, fn, defs);
                        if (recvRoot < defs.size() && defs[recvRoot].block != il::kNoBlock &&
                            defs[recvRoot].block < ld.inLoop.size() &&
                            ld.inLoop[defs[recvRoot].block]) {
                            continue;
                        }

                        bool loopSafe = true;
                        for (il::BlockId lb : ld.loopBlocks) {
                            if (fn.blocks[lb].handler != il::kNoBlock) {
                                loopSafe = false;
                                break;
                            }
                            for (const il::Instruction& loopInst : fn.blocks[lb].instructions) {
                                if (!isSafeInstForInBoundsLoop(loopInst, recv, fn, defs,
                                                               lengthKeyIndex, isAccessorKey,
                                                               protoPatchFree)) {
                                    loopSafe = false;
                                    break;
                                }
                            }
                            if (!loopSafe) break;
                        }
                        if (loopSafe) {
                            valid = true;
                            break;
                        }
                    }
                }
                if (!valid) {
                    inst.immI32 &= ~static_cast<int32_t>(il::kElemFlagProvenInBounds);
                }
            }
        }
    }
}

}  // namespace bronze::lower
