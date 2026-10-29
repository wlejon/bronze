#include "il/verifier_proofs.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "il/il_pin.h"

namespace bronze::il {

namespace {

struct DefInfo {
    Type type = Type::Void;
    size_t blockIdx = 0;
    int instIdx = -1;  // -1 for function or block parameter
    const Instruction* inst = nullptr;
};

bool isKnownNonNumberProducer(const Instruction& def) {
    switch (def.op) {
        case Op::ConstNull:
        case Op::ConstUndefined:
        case Op::ConstBool:
        case Op::ConstBigInt:
        case Op::CreateObject:
        case Op::CreateArray:
        case Op::CreateFunction:
        case Op::EnvCreate:
        case Op::TemplateObject:
        case Op::TemplateCached:
        case Op::ConcatBegin:
        case Op::ConcatAppend:
        case Op::ConcatEnd:
        case Op::ToStr:
            return true;
        case Op::Box:
            return def.boxType != Type::F64 && def.boxType != Type::I32;
        default:
            return false;
    }
}

bool isKnownNonNumberNonNullishProducer(const Instruction& def) {
    switch (def.op) {
        case Op::ConstBool:
        case Op::ConstBigInt:
        case Op::CreateObject:
        case Op::CreateArray:
        case Op::CreateFunction:
        case Op::EnvCreate:
        case Op::TemplateObject:
        case Op::TemplateCached:
        case Op::ConcatBegin:
        case Op::ConcatAppend:
        case Op::ConcatEnd:
        case Op::ToStr:
            return true;
        case Op::Box:
            return def.boxType == Type::Bool || def.boxType == Type::Str;
        default:
            return false;
    }
}

}  // namespace

bool isKnownNonArrayProducer(const Instruction& def) {
    switch (def.op) {
        case Op::ConstNull:
        case Op::ConstUndefined:
        case Op::ConstBool:
        case Op::ConstBigInt:
        case Op::CreateObject:
        case Op::CreateFunction:
        case Op::EnvCreate:
        case Op::Box:
        case Op::TemplateCached:
        case Op::ConcatBegin:
        case Op::ConcatAppend:
        case Op::ConcatEnd:
        case Op::ToStr:
            return true;
        default:
            return false;
    }
}

namespace {

bool isValidPinBarrier(int32_t raw) {
    return raw == static_cast<int32_t>(PinBarrier::Number) ||
           raw == static_cast<int32_t>(PinBarrier::NumberOrNullish) ||
           raw == static_cast<int32_t>(PinBarrier::DenseArray);
}

bool isValidTypedElemKind(int32_t kind) {
    constexpr int32_t kAllowedMask =
        kElemKindMask | kElemFlagProvenIntegralIndex | kElemFlagProvenInBounds;
    if ((kind & ~kAllowedMask) != 0) return false;
    const int32_t raw = kind & kElemKindMask;
    return (raw >= 0 && raw <= 11) || raw == kElemKindPlainArrayF64;
}

std::vector<BlockId> computeDominators(const Function& fn, std::vector<uint32_t>& rpoIndexOut) {
    const size_t numBlocks = fn.blocks.size();
    std::vector<std::vector<BlockId>> succs(numBlocks);
    std::vector<std::vector<BlockId>> preds(numBlocks);

    auto addEdge = [&](BlockId from, BlockId to) {
        if (to == kNoBlock || to >= numBlocks) return;
        auto& s = succs[from];
        if (std::find(s.begin(), s.end(), to) == s.end()) s.push_back(to);
        auto& p = preds[to];
        if (std::find(p.begin(), p.end(), from) == p.end()) p.push_back(from);
    };

    for (size_t b = 0; b < numBlocks; ++b) {
        const auto& blk = fn.blocks[b];
        if (blk.handler != kNoBlock) {
            addEdge(static_cast<BlockId>(b), blk.handler);
        }
        if (!blk.instructions.empty()) {
            const auto& term = blk.instructions.back();
            if (term.op == Op::Jump) {
                addEdge(static_cast<BlockId>(b), term.target.block);
            } else if (term.op == Op::Branch) {
                addEdge(static_cast<BlockId>(b), term.target.block);
                addEdge(static_cast<BlockId>(b), term.elseTarget.block);
            }
        }
    }

    std::vector<uint8_t> seen(numBlocks, 0);
    std::vector<BlockId> postorder;
    std::vector<std::pair<BlockId, size_t>> dfsStack;
    if (numBlocks > 0) {
        seen[0] = 1;
        dfsStack.push_back({0, 0});
    }
    while (!dfsStack.empty()) {
        auto& [b, next] = dfsStack.back();
        if (next < succs[b].size()) {
            const BlockId s = succs[b][next++];
            if (!seen[s]) {
                seen[s] = 1;
                dfsStack.push_back({s, 0});
            }
            continue;
        }
        postorder.push_back(b);
        dfsStack.pop_back();
    }

    std::vector<BlockId> rpo(postorder.rbegin(), postorder.rend());
    rpoIndexOut.assign(numBlocks, UINT32_MAX);
    for (uint32_t i = 0; i < rpo.size(); ++i) {
        rpoIndexOut[rpo[i]] = i;
    }

    std::vector<BlockId> idom(numBlocks, kNoBlock);
    if (numBlocks > 0) idom[0] = 0;

    auto intersectIdom = [&](BlockId b1, BlockId b2) -> BlockId {
        while (b1 != b2) {
            while (rpoIndexOut[b1] > rpoIndexOut[b2]) {
                b1 = idom[b1];
                if (b1 == kNoBlock) return b2;
            }
            while (rpoIndexOut[b2] > rpoIndexOut[b1]) {
                b2 = idom[b2];
                if (b2 == kNoBlock) return b1;
            }
        }
        return b1;
    };

    bool domChanged = true;
    while (domChanged) {
        domChanged = false;
        for (BlockId b : rpo) {
            if (b == 0) continue;
            BlockId newIdom = kNoBlock;
            for (BlockId p : preds[b]) {
                if (rpoIndexOut[p] == UINT32_MAX || idom[p] == kNoBlock) continue;
                newIdom = (newIdom == kNoBlock) ? p : intersectIdom(newIdom, p);
            }
            if (newIdom != kNoBlock && idom[b] != newIdom) {
                idom[b] = newIdom;
                domChanged = true;
            }
        }
    }
    if (numBlocks > 0) idom[0] = kNoBlock;
    return idom;
}

bool dominatesBlock(const std::vector<BlockId>& idom, const std::vector<uint32_t>& rpoIndex,
                    BlockId a, BlockId b) {
    if (a == b) return true;
    if (b >= idom.size() || rpoIndex[b] == UINT32_MAX) return false;
    for (BlockId cur = idom[b]; cur != kNoBlock; cur = idom[cur]) {
        if (cur == a) return true;
    }
    return false;
}

}  // namespace

bool verifyFunctionProofs(const Function& fn, const Module* module, DiagnosticSink& diags) {
    auto fail = [&](const std::string& msg) {
        diags.error(Span{}, "proof certificate refused in function " + fn.name + ": " + msg);
        return false;
    };

    // 1. Verify pinned parameter and return certificates on the function signature.
    for (size_t i = 0; i < fn.params.size(); ++i) {
        const Param& p = fn.params[i];
        if (!p.pinned) continue;
        if (p.type != Type::F64) {
            return fail("pinned parameter '" + p.name + "' has type " +
                        std::string(typeName(p.type)) + ", not f64");
        }
        if (module != nullptr && !module->keyConstants.empty() &&
            p.pinKeyIndex >= module->keyConstants.size()) {
            return fail("pinned parameter '" + p.name + "' names key index " +
                        std::to_string(p.pinKeyIndex) + ", past the module's key count " +
                        std::to_string(module->keyConstants.size()));
        }
    }
    if (fn.returnPinned) {
        if (fn.returnType != Type::F64) {
            return fail("returnPinned is set on function with return type " +
                        std::string(typeName(fn.returnType)) + ", not f64");
        }
        if (module != nullptr && !module->keyConstants.empty() &&
            fn.returnPinKeyIndex >= module->keyConstants.size()) {
            return fail("returnPinned names key index " + std::to_string(fn.returnPinKeyIndex) +
                        ", past the module's key count " +
                        std::to_string(module->keyConstants.size()));
        }
    }

    if (fn.isExternal || fn.blocks.empty()) return true;

    // 2. Collect SSA definitions.
    std::unordered_map<ValueId, DefInfo> defs;
    for (size_t i = 0; i < fn.params.size(); ++i) {
        defs[static_cast<ValueId>(i)] = DefInfo{fn.params[i].type, 0, -1, nullptr};
    }
    for (size_t bIdx = 0; bIdx < fn.blocks.size(); ++bIdx) {
        const Block& blk = fn.blocks[bIdx];
        for (const BlockParam& bp : blk.params) {
            defs[bp.id] = DefInfo{bp.type, bIdx, -1, nullptr};
        }
        for (size_t iIdx = 0; iIdx < blk.instructions.size(); ++iIdx) {
            const Instruction& inst = blk.instructions[iIdx];
            if (inst.result != kNoValue) {
                defs[inst.result] = DefInfo{inst.type, bIdx, static_cast<int>(iIdx), &inst};
            }
        }
    }

    std::vector<uint32_t> rpoIndex;
    const std::vector<BlockId> idom = computeDominators(fn, rpoIndex);

    // 3. Collect `IsNumber` branch guards and `PinGuard` barriers across the function.
    struct IsNumberBranch {
        ValueId operand = kNoValue;
        BlockId branchBlock = kNoBlock;
        BlockId thenBlock = kNoBlock;
        BlockId elseBlock = kNoBlock;
    };
    std::vector<IsNumberBranch> isNumberBranches;
    struct PinNumberSite {
        ValueId operand = kNoValue;
        size_t blockIdx = 0;
        size_t instIdx = 0;
    };
    std::vector<PinNumberSite> pinNumberSites;

    for (size_t bIdx = 0; bIdx < fn.blocks.size(); ++bIdx) {
        const Block& blk = fn.blocks[bIdx];
        for (size_t iIdx = 0; iIdx < blk.instructions.size(); ++iIdx) {
            const Instruction& inst = blk.instructions[iIdx];
            if (inst.op == Op::PinGuard && inst.operands.size() == 1 &&
                inst.immI32 == static_cast<int32_t>(PinBarrier::Number)) {
                pinNumberSites.push_back({inst.operands[0], bIdx, iIdx});
            }
        }
        if (!blk.instructions.empty() && blk.instructions.back().op == Op::Branch &&
            !blk.instructions.back().operands.empty()) {
            const Instruction& term = blk.instructions.back();
            auto condIt = defs.find(term.operands[0]);
            if (condIt != defs.end() && condIt->second.inst != nullptr &&
                condIt->second.inst->op == Op::IsNumber &&
                condIt->second.inst->operands.size() == 1) {
                isNumberBranches.push_back({condIt->second.inst->operands[0],
                                            static_cast<BlockId>(bIdx),
                                            term.target.block,
                                            term.elseTarget.block});
            }
        }
    }

    auto hasPinNumberBarrierBefore = [&](ValueId v, size_t useBlock, size_t useInst) -> bool {
        for (const PinNumberSite& site : pinNumberSites) {
            if (site.operand != v) continue;
            if (site.blockIdx == useBlock && site.instIdx < useInst) return true;
            if (site.blockIdx != useBlock &&
                dominatesBlock(idom, rpoIndex, static_cast<BlockId>(site.blockIdx),
                               static_cast<BlockId>(useBlock))) {
                return true;
            }
        }
        return false;
    };

    // Check if an EnvSet in the same function writes a known non-Number value
    // into the same (envVal, envDepth, envIndex) without a PinGuard.
    auto hasContradictingEnvSet = [&](ValueId envVal, uint32_t envDepth,
                                      uint32_t envIndex) -> bool {
        for (size_t bIdx = 0; bIdx < fn.blocks.size(); ++bIdx) {
            const Block& blk = fn.blocks[bIdx];
            for (size_t iIdx = 0; iIdx < blk.instructions.size(); ++iIdx) {
                const Instruction& inst = blk.instructions[iIdx];
                if (inst.op != Op::EnvSet) continue;
                if (inst.operands.size() < 2) continue;
                if (inst.operands[0] != envVal || inst.envDepth != envDepth ||
                    inst.envIndex != envIndex) {
                    continue;
                }
                const ValueId stored = inst.operands[1];
                if (hasPinNumberBarrierBefore(stored, bIdx, iIdx)) continue;
                auto defIt = defs.find(stored);
                if (defIt != defs.end() && defIt->second.inst != nullptr &&
                    isKnownNonNumberProducer(*defIt->second.inst)) {
                    return true;
                }
            }
        }
        return false;
    };

    auto isDominatedByIsNumberTrue = [&](ValueId v, BlockId blockId) -> bool {
        for (const IsNumberBranch& br : isNumberBranches) {
            if (br.operand != v) continue;
            if (br.thenBlock != kNoBlock &&
                dominatesBlock(idom, rpoIndex, br.thenBlock, blockId) &&
                !dominatesBlock(idom, rpoIndex, br.elseBlock, blockId)) {
                return true;
            }
        }
        return false;
    };

    auto isDominatedByIsNumberFalse = [&](ValueId v, BlockId blockId) -> bool {
        for (const IsNumberBranch& br : isNumberBranches) {
            if (br.operand != v) continue;
            if (br.elseBlock != kNoBlock &&
                dominatesBlock(idom, rpoIndex, br.elseBlock, blockId) &&
                !dominatesBlock(idom, rpoIndex, br.thenBlock, blockId)) {
                return true;
            }
        }
        return false;
    };

    // Every use of each value, as (block, instruction index): an operand, or
    // an argument a terminator passes to a block. Built on first need.
    std::unordered_map<ValueId, std::vector<std::pair<size_t, size_t>>> uses;
    bool usesBuilt = false;
    auto buildUses = [&] {
        usesBuilt = true;
        for (size_t bIdx = 0; bIdx < fn.blocks.size(); ++bIdx) {
            const Block& blk = fn.blocks[bIdx];
            for (size_t iIdx = 0; iIdx < blk.instructions.size(); ++iIdx) {
                const Instruction& inst = blk.instructions[iIdx];
                for (ValueId v : inst.operands) uses[v].push_back({bIdx, iIdx});
                for (ValueId v : inst.target.args) uses[v].push_back({bIdx, iIdx});
                for (ValueId v : inst.elseTarget.args) uses[v].push_back({bIdx, iIdx});
            }
        }
    };
    auto isPureF64Arith = [](const Instruction& inst) {
        if (inst.type != Type::F64 || inst.result == kNoValue) return false;
        switch (inst.op) {
            case Op::Add: case Op::Sub: case Op::Neg: case Op::Mul:
            case Op::Div: case Op::Mod: case Op::Pow:
                return true;
            default:
                return false;
        }
    };
    // A raw unbox placed ahead of its test (a guarded region's run point,
    // guard_region_build.cpp): a bitcast over bits nothing has looked at
    // yet. It is certified when nothing observes them before is.number of
    // the same value has held: every use, followed through pure f64
    // arithmetic (which cannot fault or escape), lies where that test's true
    // edge dominates. A use anywhere else (a compare, a branch, a box, a
    // store, a call, a block argument) refuses it.
    auto isValidatedBeforeUse = [&](ValueId unboxed, ValueId src) -> bool {
        if (!usesBuilt) buildUses();
        std::vector<ValueId> work{unboxed};
        std::unordered_set<ValueId> seen{unboxed};
        while (!work.empty()) {
            const ValueId v = work.back();
            work.pop_back();
            auto it = uses.find(v);
            if (it == uses.end()) continue;
            for (const auto& [ub, ui] : it->second) {
                if (isDominatedByIsNumberTrue(src, static_cast<BlockId>(ub))) continue;
                const Instruction& user = fn.blocks[ub].instructions[ui];
                if (!isPureF64Arith(user)) return false;
                if (seen.insert(user.result).second) work.push_back(user.result);
            }
        }
        return true;
    };

    // 4. Verify each instruction's proof certificates in deterministic block/instruction order.
    for (size_t bIdx = 0; bIdx < fn.blocks.size(); ++bIdx) {
        const Block& blk = fn.blocks[bIdx];
        const BlockId blockId = static_cast<BlockId>(bIdx);

        for (size_t iIdx = 0; iIdx < blk.instructions.size(); ++iIdx) {
            const Instruction& inst = blk.instructions[iIdx];

            // --- (a) Op::Unbox (`rawUnbox` and `nullishUnbox`) ---
            if (inst.op == Op::Unbox) {
                if (inst.rawUnbox && inst.nullishUnbox) {
                    return fail("unbox %" + std::to_string(inst.result) +
                                " sets both rawUnbox and nullishUnbox, which are mutually exclusive");
                }
                if (inst.rawUnbox || inst.nullishUnbox) {
                    if (inst.operands.size() != 1 || inst.result == kNoValue) {
                        return fail("unchecked unbox must have 1 operand and a result");
                    }
                    if (inst.type != Type::F64) {
                        return fail("unchecked unbox %" + std::to_string(inst.result) +
                                    " has result type " + std::string(typeName(inst.type)) +
                                    ", not f64");
                    }
                    const ValueId src = inst.operands[0];
                    auto srcIt = defs.find(src);
                    if (srcIt == defs.end() || srcIt->second.type != Type::Dynamic) {
                        return fail("unchecked unbox %" + std::to_string(inst.result) +
                                    " operand %" + std::to_string(src) + " is not dynamic");
                    }

                    const DefInfo& srcDef = srcIt->second;
                    // A dominating is.number that held is the certificate,
                    // whatever the producer: under a producer that never
                    // makes a Number the unbox is unreachable (its guard
                    // folds false after lowering) and never runs.
                    if (inst.rawUnbox && (isDominatedByIsNumberTrue(src, blockId) ||
                                          isValidatedBeforeUse(inst.result, src))) {
                        // certified
                    } else if (inst.rawUnbox) {
                        if (srcDef.inst != nullptr && isKnownNonNumberProducer(*srcDef.inst)) {
                            return fail("raw unbox %" + std::to_string(inst.result) +
                                        " reads %" + std::to_string(src) + " defined by " +
                                        std::string(opName(srcDef.inst->op)) +
                                        ", which never produces a Number");
                        }
                        if (isDominatedByIsNumberFalse(src, blockId)) {
                            return fail("raw unbox %" + std::to_string(inst.result) +
                                        " of %" + std::to_string(src) + " in b" +
                                        std::to_string(bIdx) +
                                        " lies on the false branch of is.number");
                        }
                        if (srcDef.inst != nullptr &&
                            (srcDef.inst->op == Op::EnvGet || srcDef.inst->op == Op::EnvGetTdz) &&
                            !srcDef.inst->operands.empty() &&
                            hasContradictingEnvSet(srcDef.inst->operands[0],
                                                   srcDef.inst->envDepth,
                                                   srcDef.inst->envIndex)) {
                            return fail("raw unbox %" + std::to_string(inst.result) +
                                        " reads env.get (" +
                                        std::to_string(srcDef.inst->envDepth) + ", " +
                                        std::to_string(srcDef.inst->envIndex) +
                                        "), which is contradicted by a non-number env.set");
                        }
                        // Positive certificate origin check: if `src` is defined by an
                        // instruction that is neither `Box(F64|I32)` nor an audited slot/property
                        // read (`PropGet`, `ElemGet`, `EnvGet`, `EnvGetTdz`, `GlobalGet`), it
                        // MUST be certified by an `IsNumber` guard or a `PinGuard(Number)`.
                        if (srcDef.inst != nullptr) {
                            const Op defOp = srcDef.inst->op;
                            const bool inherentlyCertifiable =
                                (defOp == Op::Box &&
                                 (srcDef.inst->boxType == Type::F64 ||
                                  srcDef.inst->boxType == Type::I32)) ||
                                defOp == Op::PropGet || defOp == Op::ElemGet ||
                                defOp == Op::EnvGet || defOp == Op::EnvGetTdz ||
                                defOp == Op::GlobalGet;
                            if (!inherentlyCertifiable &&
                                !isDominatedByIsNumberTrue(src, blockId) &&
                                !hasPinNumberBarrierBefore(src, bIdx, iIdx)) {
                                return fail("raw unbox %" + std::to_string(inst.result) +
                                            " of %" + std::to_string(src) + " (defined by " +
                                            std::string(opName(defOp)) +
                                            ") has no dominating is.number or pin.guard certificate");
                            }
                        }
                    } else if (inst.nullishUnbox) {
                        if (srcDef.inst != nullptr &&
                            isKnownNonNumberNonNullishProducer(*srcDef.inst)) {
                            return fail("nullish unbox %" + std::to_string(inst.result) +
                                        " reads %" + std::to_string(src) + " defined by " +
                                        std::string(opName(srcDef.inst->op)) +
                                        ", which produces neither Number nor nullish");
                        }
                    }
                }
            }

            // --- (b) Op::PinGuard ---
            if (inst.op == Op::PinGuard) {
                if (inst.operands.size() != 1 || inst.result != kNoValue ||
                    inst.type != Type::Void) {
                    return fail("pin.guard takes exactly one operand and produces void");
                }
                const ValueId op0 = inst.operands[0];
                auto opIt = defs.find(op0);
                if (opIt == defs.end() || opIt->second.type != Type::Dynamic) {
                    return fail("pin.guard operand %" + std::to_string(op0) + " is not dynamic");
                }
                if (!isValidPinBarrier(inst.immI32)) {
                    return fail("pin.guard has invalid PinBarrier kind " +
                                std::to_string(inst.immI32));
                }
                if (module != nullptr && !module->keyConstants.empty() &&
                    inst.keyIndex >= module->keyConstants.size()) {
                    return fail("pin.guard names key index " + std::to_string(inst.keyIndex) +
                                ", past the module's key count " +
                                std::to_string(module->keyConstants.size()));
                }
            }

            // --- (c) Op::IsDenseArray ---
            if (inst.op == Op::IsDenseArray && !inst.operands.empty()) {
                const ValueId recv = inst.operands[0];
                auto recvIt = defs.find(recv);
                if (recvIt != defs.end() && recvIt->second.inst != nullptr &&
                    isKnownNonArrayProducer(*recvIt->second.inst)) {
                    return fail("is.dense_array operand %" + std::to_string(recv) +
                                " is defined by " +
                                std::string(opName(recvIt->second.inst->op)) +
                                ", which never produces an Array");
                }
            }

            // --- (d) Op::ElemGetTyped and Op::ElemSetTyped ---
            if (inst.op == Op::ElemGetTyped) {
                if (inst.operands.size() != 2 || inst.result == kNoValue ||
                    inst.type != Type::F64) {
                    return fail("elem.get.typed takes 2 operands and produces f64");
                }
                const ValueId recv = inst.operands[0];
                const ValueId idx = inst.operands[1];
                auto recvIt = defs.find(recv);
                auto idxIt = defs.find(idx);
                if (recvIt == defs.end() || recvIt->second.type != Type::Dynamic) {
                    return fail("elem.get.typed receiver %" + std::to_string(recv) +
                                " is not dynamic");
                }
                if (recvIt->second.inst != nullptr &&
                    isKnownNonArrayProducer(*recvIt->second.inst)) {
                    return fail("elem.get.typed receiver %" + std::to_string(recv) +
                                " is defined by " +
                                std::string(opName(recvIt->second.inst->op)) +
                                ", which never produces an array or typed view");
                }
                if (idxIt == defs.end() ||
                    (idxIt->second.type != Type::F64 && idxIt->second.type != Type::I32)) {
                    return fail("elem.get.typed index %" + std::to_string(idx) +
                                " is not f64 or i32");
                }
                if (!isValidTypedElemKind(inst.immI32)) {
                    return fail("elem.get.typed has invalid element kind " +
                                std::to_string(inst.immI32));
                }
            } else if (inst.op == Op::ElemSetTyped) {
                if (inst.operands.size() != 3 || inst.result != kNoValue ||
                    inst.type != Type::Void) {
                    return fail("elem.set.typed takes 3 operands and produces void");
                }
                const ValueId recv = inst.operands[0];
                const ValueId idx = inst.operands[1];
                const ValueId val = inst.operands[2];
                auto recvIt = defs.find(recv);
                auto idxIt = defs.find(idx);
                auto valIt = defs.find(val);
                if (recvIt == defs.end() || recvIt->second.type != Type::Dynamic) {
                    return fail("elem.set.typed receiver %" + std::to_string(recv) +
                                " is not dynamic");
                }
                if (recvIt->second.inst != nullptr &&
                    isKnownNonArrayProducer(*recvIt->second.inst)) {
                    return fail("elem.set.typed receiver %" + std::to_string(recv) +
                                " is defined by " +
                                std::string(opName(recvIt->second.inst->op)) +
                                ", which never produces an array or typed view");
                }
                if (idxIt == defs.end() ||
                    (idxIt->second.type != Type::F64 && idxIt->second.type != Type::I32)) {
                    return fail("elem.set.typed index %" + std::to_string(idx) +
                                " is not f64 or i32");
                }
                if (valIt == defs.end() || valIt->second.type != Type::F64) {
                    return fail("elem.set.typed value %" + std::to_string(val) + " is not f64");
                }
                if (!isValidTypedElemKind(inst.immI32)) {
                    return fail("elem.set.typed has invalid element kind " +
                                std::to_string(inst.immI32));
                }
            }
        }
    }

    return true;
}

bool verifyModuleProofs(const Module& module, DiagnosticSink& diags) {
    if (!verifyTypedEntries(module, diags)) return false;
    for (const Function& fn : module.functions) {
        if (!verifyFunctionProofs(fn, &module, diags)) {
            return false;
        }
    }
    return true;
}

}  // namespace bronze::il
