#include "lower/typed_body_version.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "abi/bronze_abi.h"
#include "il/il_pin.h"
#include "il/verifier_proofs.h"
#include "lower/guard_region_cfg.h"
#include "lower/typed_body_version_plan.h"

namespace bronze::lower {

namespace {

// The body, in IL instructions. It is copied whole, so this bounds what the
// pass adds to a function; slerpFlat's fast copy is about 270.
constexpr size_t kMaxBodyInstructions = 400;
// Below this many flagged accesses the entry tests and the second copy cost
// more than the accesses save.
constexpr size_t kMinAccesses = 4;
// Caps in-body `is.number` split points so interleaved property reads cannot
// explode block count.
constexpr size_t kMaxGuardSplits = 24;

std::vector<il::Type> valueTypes(const il::Function& fn) {
    std::vector<il::Type> t(fn.valueCount, il::Type::Void);
    for (size_t i = 0; i < fn.params.size() && i < t.size(); ++i) t[i] = fn.params[i].type;
    for (const auto& b : fn.blocks) {
        for (const auto& p : b.params) {
            if (p.id < t.size()) t[p.id] = p.type;
        }
        for (const auto& inst : b.instructions) {
            if (inst.result != il::kNoValue && inst.result < t.size()) t[inst.result] = inst.type;
        }
    }
    return t;
}

// A straight-line function is all one entry block: its code moves to a block
// of its own, entered by a jump the versioning can split.
bool splitEntry(il::Function& fn) {
    const Cfg cfg = buildCfg(fn);
    if (!cfg.preds[0].empty()) return false;
    for (uint32_t r : cfg.rpoIndex) {
        if (r == UINT32_MAX) return false;
    }
    il::Block body;
    body.id = static_cast<il::BlockId>(fn.blocks.size());
    body.copyClass = fn.blocks[0].copyClass;
    body.copyRegion = fn.blocks[0].copyRegion;
    body.handler = fn.blocks[0].handler;
    body.instructions = std::move(fn.blocks[0].instructions);
    il::Instruction jump;
    jump.op = il::Op::Jump;
    jump.type = il::Type::Void;
    jump.result = il::kNoValue;
    jump.target = il::BlockTarget{body.id, {}};
    if (!body.instructions.empty()) jump.span = body.instructions.front().span;
    fn.blocks[0].instructions = {std::move(jump)};
    fn.blocks.push_back(std::move(body));
    return true;
}

bool selectTypedBodyPlan(const il::Function& fn, const Cfg& cfg, TypedBodyPlan& plan) {
    const il::Block& b0 = fn.blocks[0];
    if (!cfg.preds[0].empty()) return false;
    const il::BlockId entry = b0.instructions.back().target.block;
    if (entry == 0 || entry >= fn.blocks.size() || cfg.preds[entry].size() != 1) return false;
    plan.entry = entry;

    plan.in.assign(fn.blocks.size(), 0);
    size_t size = 0;
    for (il::BlockId b = 0; b < fn.blocks.size(); ++b) {
        if (cfg.rpoIndex[b] == UINT32_MAX || !cfg.dominates(entry, b)) continue;
        if (fn.blocks[b].copyClass == il::CopyClass::Slow) continue;
        plan.in[b] = 1;
        plan.body.push_back(b);
        size += fn.blocks[b].instructions.size();
    }
    if (size > kMaxBodyInstructions) return false;
    // RPO order puts `entry` first and every predecessor in the loop-free body
    // ahead of its successors.
    std::sort(plan.body.begin(), plan.body.end(),
              [&](il::BlockId a, il::BlockId b) { return cfg.rpoIndex[a] < cfg.rpoIndex[b]; });

    for (il::BlockId b : plan.body) {
        const il::Block& blk = fn.blocks[b];
        if (blk.handler != il::kNoBlock) return false;
        if (!blk.instructions.empty() && blk.instructions.front().op == il::Op::ExcTake) return false;
        for (il::BlockId p : cfg.preds[b]) {
            if (b != entry && !plan.in[p]) return false;
            if (plan.in[p] && cfg.dominates(b, p)) return false;
        }
    }

    // Nothing outside the body may read a value it defines, other than through
    // the arguments its own jumps pass: the copy gives each such value a
    // second definition.
    plan.defs = computeDefSites(fn);
    const auto definedIn = [&](il::ValueId v) {
        return v < plan.defs.size() && plan.defs[v].block != il::kNoBlock && plan.in[plan.defs[v].block];
    };
    for (const auto& blk : fn.blocks) {
        if (plan.in[blk.id]) continue;
        for (const auto& inst : blk.instructions) {
            for (il::ValueId v : inst.operands) {
                if (definedIn(v)) return false;
            }
            for (il::ValueId v : inst.target.args) {
                if (definedIn(v)) return false;
            }
            for (il::ValueId v : inst.elseTarget.args) {
                if (definedIn(v)) return false;
            }
        }
    }

    plan.types = valueTypes(fn);
    const auto defInst = [&](il::ValueId v) -> const il::Instruction* {
        if (v >= plan.defs.size() || plan.defs[v].block == il::kNoBlock ||
            plan.defs[v].index == UINT32_MAX) {
            return nullptr;
        }
        return &fn.blocks[plan.defs[v].block].instructions[plan.defs[v].index];
    };

    std::vector<il::ValueId> proven;
    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.instructions) {
            if ((inst.op == il::Op::ElemGetTyped || inst.op == il::Op::ElemSetTyped) && !inst.operands.empty()) {
                proven.push_back(inst.operands[0]);
            }
        }
    }
    const auto receiverOk = [&](il::ValueId r) {
        if (r >= plan.types.size() || plan.types[r] != il::Type::Dynamic || definedIn(r)) return false;
        if (std::find(proven.begin(), proven.end(), r) != proven.end()) return false;
        const il::Instruction* d = defInst(r);
        return !(d && il::isKnownNonArrayProducer(*d));
    };

    std::unordered_map<il::ValueId, uint32_t> useCount;
    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.instructions) {
            for (il::ValueId v : inst.operands) ++useCount[v];
            for (il::ValueId v : inst.target.args) ++useCount[v];
            for (il::ValueId v : inst.elseTarget.args) ++useCount[v];
        }
    }
    const auto fieldCopy = [&](const il::Instruction& inst) {
        if (inst.op == il::Op::ElemGet) {
            if (inst.result == il::kNoValue || useCount[inst.result] != 1) return false;
            for (il::BlockId b : plan.body) {
                for (const auto& u : fn.blocks[b].instructions) {
                    if (u.op == il::Op::PropSet && u.operands.size() >= 2 && u.operands[1] == inst.result) {
                        return true;
                    }
                }
            }
            return false;
        }
        const il::ValueId v = inst.operands.size() >= 3 ? inst.operands[2] : il::kNoValue;
        const il::Instruction* d = defInst(v);
        return d && d->op == il::Op::PropGet;
    };

    size_t accesses = 0;
    size_t copies = 0;
    bool allStores = true;
    for (il::BlockId b : plan.body) {
        for (const auto& inst : fn.blocks[b].instructions) {
            if (!isBodyElemAccess(inst) || !receiverOk(inst.operands[0])) continue;
            ++accesses;
            if (inst.op != il::Op::ElemSet) allStores = false;
            if (fieldCopy(inst)) ++copies;
            if (std::find(plan.receivers.begin(), plan.receivers.end(), inst.operands[0]) ==
                plan.receivers.end()) {
                plan.receivers.push_back(inst.operands[0]);
            }
        }
    }
    if (accesses < kMinAccesses || accesses == copies) return false;
    std::sort(plan.receivers.begin(), plan.receivers.end());

    const auto isFlaggedAccess = [&](const il::Instruction& inst) {
        return isBodyElemAccess(inst) &&
               std::binary_search(plan.receivers.begin(), plan.receivers.end(), inst.operands[0]);
    };

    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == il::Op::Unbox && inst.rawUnbox && inst.type == il::Type::F64 &&
                inst.operands.size() == 1 && inst.result != il::kNoValue) {
                plan.rawUnboxes[inst.operands[0]].push_back(inst.result);
            }
        }
    }
    const auto rawUnboxAt = [&](il::ValueId v, il::BlockId b, uint32_t idx) {
        return bodyDominatingRawUnboxAt(cfg, plan.defs, plan.rawUnboxes, v, b, idx);
    };
    const auto isNonNumberValue = [&](il::ValueId v) {
        const il::Instruction* d = defInst(v);
        return d && isBodyKnownNonNumberDef(*d);
    };
    const auto isBoxedNumber = [&](il::ValueId v) {
        const il::Instruction* d = defInst(v);
        return d && d->op == il::Op::Box && (d->boxType == il::Type::F64 || d->boxType == il::Type::I32) &&
               d->operands.size() == 1;
    };

    // Identify Dynamic `Op::Add`s in `body` that act as numeric addition rather
    // than string concatenation.
    const auto isNumericProducer = [&](il::ValueId v, il::BlockId atBlock, uint32_t atIdx) {
        if (v < plan.types.size() && (plan.types[v] == il::Type::F64 || plan.types[v] == il::Type::I32)) {
            return true;
        }
        if (isBoxedNumber(v) || plan.numericAdd.count(v) != 0) return true;
        if (rawUnboxAt(v, atBlock, atIdx) != il::kNoValue) return true;
        const il::Instruction* d = defInst(v);
        return d && (isBodyFloatArithOp(d->op) || isBodyBitwiseBinOp(d->op) || d->op == il::Op::ToInt32);
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (il::BlockId b : plan.body) {
            const auto& insts = fn.blocks[b].instructions;
            for (uint32_t i = 0; i < insts.size(); ++i) {
                const il::Instruction& inst = insts[i];
                if (inst.op != il::Op::Add || inst.type != il::Type::Dynamic || inst.operands.size() != 2 ||
                    inst.result == il::kNoValue || plan.numericAdd.count(inst.result) != 0) {
                    continue;
                }
                if (isNonNumberValue(inst.operands[0]) || isNonNumberValue(inst.operands[1])) continue;
                bool ok = isNumericProducer(inst.operands[0], b, i) ||
                          isNumericProducer(inst.operands[1], b, i);
                if (!ok && useCount[inst.result] > 0) {
                    bool allNumericUses = true;
                    for (il::BlockId ub : plan.body) {
                        for (const auto& u : fn.blocks[ub].instructions) {
                            for (size_t oi = 0; oi < u.operands.size(); ++oi) {
                                if (u.operands[oi] != inst.result) continue;
                                const bool numUse =
                                    (isFlaggedAccess(u) && (oi == 1 || oi == 2)) ||
                                    (u.op == il::Op::Unbox && u.type == il::Type::F64) ||
                                    u.op == il::Op::ToInt32 || isBodyFloatArithOp(u.op) ||
                                    isBodyBitwiseBinOp(u.op) ||
                                    (u.op == il::Op::Add && plan.numericAdd.count(u.result) != 0);
                                if (!numUse) allNumericUses = false;
                            }
                            for (il::ValueId a : u.target.args) {
                                if (a == inst.result) allNumericUses = false;
                            }
                            for (il::ValueId a : u.elseTarget.args) {
                                if (a == inst.result) allNumericUses = false;
                            }
                        }
                    }
                    ok = allNumericUses;
                }
                if (ok) {
                    plan.numericAdd.insert(inst.result);
                    changed = true;
                }
            }
        }
    }

    // Vertex-batcher specialization (`packQuadAttributes`): two write-only
    // receivers, one fed float arithmetic and no bitwise ops, the other fed
    // bitwise integer packing and no float arithmetic, specialize directly to
    // Float32Array + Uint32Array so stores lower via ElemSetTyped.
    plan.receiverKinds.assign(plan.receivers.size(), il::kTypedArrayWordView);
    bool concreteKinds = false;
    if (allStores && plan.receivers.size() == 2) {
        size_t floatStores[2] = {0, 0};
        size_t bitwiseStores[2] = {0, 0};
        bool nonNumOperand = false;
        for (il::BlockId b : plan.body) {
            for (const auto& inst : fn.blocks[b].instructions) {
                if (!isFlaggedAccess(inst)) continue;
                const size_t rIdx = (inst.operands[0] == plan.receivers[0]) ? 0 : 1;
                if (isNonNumberValue(inst.operands[1]) || isNonNumberValue(inst.operands[2])) {
                    nonNumOperand = true;
                }
                const il::Instruction* vd = defInst(inst.operands[2]);
                if (vd && vd->op == il::Op::Box && vd->operands.size() == 1) vd = defInst(vd->operands[0]);
                if (!vd) continue;
                if (vd->op == il::Op::Add ? (vd->type == il::Type::F64 || plan.numericAdd.count(vd->result) != 0)
                                          : isBodyFloatArithOp(vd->op)) {
                    ++floatStores[rIdx];
                } else if (isBodyBitwiseBinOp(vd->op) || vd->op == il::Op::ToInt32 ||
                           vd->op == il::Op::ConstI32) {
                    ++bitwiseStores[rIdx];
                }
            }
        }
        if (!nonNumOperand) {
            if (floatStores[0] > 0 && bitwiseStores[0] == 0 && bitwiseStores[1] > 0 && floatStores[1] == 0) {
                concreteKinds = true;
                plan.receiverKinds[0] = BRONZE_ABI_TA_KIND_FLOAT32;
                plan.receiverKinds[1] = BRONZE_ABI_TA_KIND_UINT32;
            } else if (floatStores[1] > 0 && bitwiseStores[1] == 0 && bitwiseStores[0] > 0 &&
                       floatStores[0] == 0) {
                concreteKinds = true;
                plan.receiverKinds[1] = BRONZE_ABI_TA_KIND_FLOAT32;
                plan.receiverKinds[0] = BRONZE_ABI_TA_KIND_UINT32;
            }
        }
    }

    // Pre-body Dynamic values (`index`, `textureId`, `dstOffset`, `t`) used as
    // numeric operands inside `body` are guarded once in `enterThrough`.
    const auto canGuardPreBody = [&](il::ValueId v) {
        if (v >= plan.types.size() || plan.types[v] != il::Type::Dynamic || definedIn(v)) return false;
        if (std::binary_search(plan.receivers.begin(), plan.receivers.end(), v)) return false;
        if (isBoxedNumber(v) || isNonNumberValue(v)) return false;
        return rawUnboxAt(v, entry, 0) == il::kNoValue;
    };
    for (il::BlockId b : plan.body) {
        for (const auto& inst : fn.blocks[b].instructions) {
            const bool prom = isBodyPromotableDynOp(fn, plan, inst) ||
                              (inst.op == il::Op::Unbox && !inst.rawUnbox && !inst.nullishUnbox &&
                               inst.type == il::Type::F64) ||
                              inst.op == il::Op::ToInt32 ||
                              (concreteKinds && isFlaggedAccess(inst));
            if (!prom) continue;
            for (size_t oi = 0; oi < inst.operands.size(); ++oi) {
                if (concreteKinds && isFlaggedAccess(inst) && oi == 0) continue;
                const il::ValueId v = inst.operands[oi];
                if (canGuardPreBody(v)) plan.guardedPreBody.push_back(v);
            }
        }
    }
    std::sort(plan.guardedPreBody.begin(), plan.guardedPreBody.end());
    plan.guardedPreBody.erase(std::unique(plan.guardedPreBody.begin(), plan.guardedPreBody.end()),
                              plan.guardedPreBody.end());

    // Spine blocks dominate every block in `body` reachable from them.
    std::vector<uint8_t> isSpine(fn.blocks.size(), 0);
    for (il::BlockId s : plan.body) {
        bool hasAheadRawUnbox = false;
        for (const auto& inst : fn.blocks[s].instructions) {
            if (inst.op == il::Op::Unbox && inst.rawUnbox) {
                hasAheadRawUnbox = true;
                break;
            }
        }
        if (hasAheadRawUnbox) continue;

        std::vector<uint8_t> reach(fn.blocks.size(), 0);
        std::vector<il::BlockId> stack{s};
        reach[s] = 1;
        bool dominatesAll = true;
        while (!stack.empty()) {
            const il::BlockId cur = stack.back();
            stack.pop_back();
            if (!cfg.dominates(s, cur)) {
                dominatesAll = false;
                break;
            }
            for (il::BlockId nx : cfg.succs[cur]) {
                if (nx < plan.in.size() && plan.in[nx] && !reach[nx]) {
                    reach[nx] = 1;
                    stack.push_back(nx);
                }
            }
        }
        if (dominatesAll) isSpine[s] = 1;
    }

    std::unordered_set<il::ValueId> wantedInBody;
    const auto canGuardInSpine = [&](il::ValueId v, il::BlockId defBlk, uint32_t defIdx) {
        if (v >= plan.types.size() || plan.types[v] != il::Type::Dynamic) return false;
        if (!definedIn(v) || !isSpine[defBlk] || defIdx == UINT32_MAX) return false;
        if (isBoxedNumber(v) || isNonNumberValue(v)) return false;
        return !isBodyPromotableDynOp(fn, plan, fn.blocks[defBlk].instructions[defIdx]);
    };
    for (il::BlockId b : plan.body) {
        for (uint32_t i = 0; i < fn.blocks[b].instructions.size(); ++i) {
            const il::Instruction& inst = fn.blocks[b].instructions[i];
            const bool numericOp =
                isBodyPromotableDynOp(fn, plan, inst) || inst.op == il::Op::ToInt32 ||
                (inst.op == il::Op::Unbox && !inst.rawUnbox && !inst.nullishUnbox &&
                 inst.type == il::Type::F64);
            if (numericOp) {
                for (il::ValueId v : inst.operands) {
                    if (v < plan.defs.size() && canGuardInSpine(v, plan.defs[v].block, plan.defs[v].index) &&
                        rawUnboxAt(v, b, i) == il::kNoValue) {
                        wantedInBody.insert(v);
                    }
                }
            }
            if (concreteKinds && isFlaggedAccess(inst)) {
                for (size_t oi : {size_t{1}, size_t{2}}) {
                    const il::ValueId v = inst.operands[oi];
                    if (v < plan.defs.size() && canGuardInSpine(v, plan.defs[v].block, plan.defs[v].index) &&
                        rawUnboxAt(v, b, i) == il::kNoValue) {
                        wantedInBody.insert(v);
                    }
                }
            }
            if (isFlaggedAccess(inst) && inst.op == il::Op::ElemSet && inst.operands.size() >= 3) {
                const il::ValueId v = inst.operands[2];
                const il::Instruction* vd = defInst(v);
                if (vd && vd->op == il::Op::ElemGet && plan.defs[v].block == entry &&
                    isFlaggedAccess(*vd) && canGuardInSpine(v, plan.defs[v].block, plan.defs[v].index) &&
                    rawUnboxAt(v, b, i) == il::kNoValue) {
                    wantedInBody.insert(v);
                }
            }
        }
    }

    plan.splitsOf.assign(fn.blocks.size(), {});
    for (il::BlockId b : plan.body) {
        if (!isSpine[b]) continue;
        const auto& insts = fn.blocks[b].instructions;
        std::vector<uint8_t> canSplit(insts.size(), 1);
        if (!insts.empty() && insts[0].op == il::Op::ClosureEnv) canSplit[0] = 0;
        int concatDepth = 0;
        for (uint32_t i = 0; i < insts.size(); ++i) {
            if (concatDepth > 0) canSplit[i] = 0;
            if (insts[i].op == il::Op::ConcatBegin) ++concatDepth;
            if (insts[i].op == il::Op::ConcatEnd && concatDepth > 0) --concatDepth;
        }

        std::vector<il::ValueId> avail;
        std::unordered_set<il::ValueId> guardedHere;
        uint32_t lastAvailDef = UINT32_MAX;
        for (uint32_t i = 0; i < insts.size(); ++i) {
            const il::Instruction& inst = insts[i];
            bool needSplit = false;
            if (canSplit[i] && plan.totalSplits < kMaxGuardSplits) {
                const bool checksOps =
                    isBodyPromotableDynOp(fn, plan, inst) || inst.op == il::Op::ToInt32 ||
                    (concreteKinds && isFlaggedAccess(inst)) ||
                    (inst.op == il::Op::Unbox && !inst.rawUnbox && !inst.nullishUnbox &&
                     inst.type == il::Type::F64) ||
                    (isFlaggedAccess(inst) && inst.op == il::Op::ElemSet);
                if (checksOps) {
                    for (size_t oi = 0; oi < inst.operands.size(); ++oi) {
                        if (isFlaggedAccess(inst) && oi == 0) continue;
                        const il::ValueId v = inst.operands[oi];
                        if (std::find(avail.begin(), avail.end(), v) != avail.end() &&
                            guardedHere.count(v) == 0) {
                            needSplit = true;
                            break;
                        }
                    }
                }
            }
            if (needSplit) {
                TypedBodyGuardSplit sp;
                sp.index = i;
                for (il::ValueId v : avail) {
                    if (guardedHere.insert(v).second) sp.guarded.push_back(v);
                }
                if (!sp.guarded.empty()) {
                    plan.splitsOf[b].push_back(std::move(sp));
                    ++plan.totalSplits;
                }
            }
            if (inst.result != il::kNoValue && wantedInBody.count(inst.result) != 0) {
                avail.push_back(inst.result);
                lastAvailDef = i;
            }
        }
        if (plan.totalSplits < kMaxGuardSplits && lastAvailDef != UINT32_MAX &&
            lastAvailDef + 1 < insts.size()) {
            std::vector<il::ValueId> rem;
            for (il::ValueId v : avail) {
                if (guardedHere.count(v) == 0) rem.push_back(v);
            }
            if (!rem.empty()) {
                uint32_t splitIdx = lastAvailDef + 1;
                while (splitIdx < insts.size() && !canSplit[splitIdx]) ++splitIdx;
                if (splitIdx < insts.size()) {
                    TypedBodyGuardSplit sp;
                    sp.index = splitIdx;
                    sp.guarded = std::move(rem);
                    plan.splitsOf[b].push_back(std::move(sp));
                    ++plan.totalSplits;
                }
            }
        }
    }
    return true;
}

bool runTypedBodyVersion(il::Function& fn, std::vector<il::BlockId>& coldOut) {
    if (fn.blocks.empty()) return false;
    size_t candidates = 0;
    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.instructions) candidates += isBodyElemAccess(inst) ? 1 : 0;
    }
    if (candidates < kMinAccesses) return false;

    const il::Function original = fn;
    const il::Block& first = fn.blocks[0];
    const bool split = first.instructions.empty() || first.instructions.back().op != il::Op::Jump;
    if (split && !splitEntry(fn)) {
        fn = original;
        return false;
    }
    const Cfg cfg = buildCfg(fn);
    TypedBodyPlan plan;
    if (!selectTypedBodyPlan(fn, cfg, plan) || !emitTypedBodyVersion(fn, cfg, plan, coldOut)) {
        fn = original;
        coldOut.clear();
        return false;
    }
    return true;
}

}  // namespace

bool applyTypedBodyVersions(il::Module& module, ColdBlocks* cold) {
    bool any = false;
    for (il::Function& fn : module.functions) {
        if (fn.isExternal || fn.blocks.empty() || fn.isCoroutineBody()) continue;
        std::vector<il::BlockId> coldBlocks;
        if (!runTypedBodyVersion(fn, coldBlocks)) continue;
        any = true;
        if (cold) {
            auto& blocks = (*cold)[fn.name];
            blocks.insert(blocks.end(), coldBlocks.begin(), coldBlocks.end());
        }
    }
    return any;
}

}  // namespace bronze::lower
