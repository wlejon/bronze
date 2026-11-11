#include "lower/typed_body_version.h"

#include <algorithm>
#include <cmath>
#include <map>
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
constexpr size_t kMaxGuardSplits = 64;

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
    bool hasStores = false;
    for (il::BlockId b : plan.body) {
        for (const auto& inst : fn.blocks[b].instructions) {
            if (!isBodyElemAccess(inst) || !receiverOk(inst.operands[0])) continue;
            ++accesses;
            if (inst.op == il::Op::ElemSet) {
                hasStores = true;
            } else {
                allStores = false;
            }
            if (fieldCopy(inst)) ++copies;
            if (std::find(plan.receivers.begin(), plan.receivers.end(), inst.operands[0]) ==
                plan.receivers.end()) {
                plan.receivers.push_back(inst.operands[0]);
            }
        }
    }
    if (accesses < kMinAccesses || accesses == copies) return false;
    std::sort(plan.receivers.begin(), plan.receivers.end());

    const auto receiverIndex = [&](il::ValueId r) -> size_t {
        const auto it = std::lower_bound(plan.receivers.begin(), plan.receivers.end(), r);
        return (it != plan.receivers.end() && *it == r)
                   ? static_cast<size_t>(it - plan.receivers.begin())
                   : SIZE_MAX;
    };
    const auto isFlaggedAccess = [&](const il::Instruction& inst) {
        return isBodyElemAccess(inst) && receiverIndex(inst.operands[0]) != SIZE_MAX;
    };
    const auto isProvenFloatRead = [&](const il::Instruction& inst) {
        if (inst.op != il::Op::ElemGet || !isBodyElemAccess(inst)) return false;
        const size_t rIdx = receiverIndex(inst.operands[0]);
        return rIdx < plan.provenInBoundsRead.size() && plan.provenInBoundsRead[rIdx] != 0;
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
    const auto canGuardPreBody = [&](il::ValueId v) {
        if (v >= plan.types.size() || plan.types[v] != il::Type::Dynamic || definedIn(v)) return false;
        if (receiverIndex(v) != SIZE_MAX) return false;
        if (isBoxedNumber(v) || isNonNumberValue(v)) return false;
        return rawUnboxAt(v, entry, 0) == il::kNoValue;
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
        if (d && isProvenFloatRead(*d)) return true;
        return d && (isBodyFloatArithOp(d->op) || isBodyBitwiseBinOp(d->op) ||
                     d->op == il::Op::MathUnary || d->op == il::Op::ToInt32);
    };
    const auto propagateNumericAdd = [&]() {
        for (bool changed = true; changed;) {
            changed = false;
            for (il::BlockId b : plan.body) {
                const auto& insts = fn.blocks[b].instructions;
                for (uint32_t i = 0; i < insts.size(); ++i) {
                    const il::Instruction& inst = insts[i];
                    if (inst.op != il::Op::Add || inst.type != il::Type::Dynamic ||
                        inst.operands.size() != 2 || inst.result == il::kNoValue ||
                        plan.numericAdd.count(inst.result) != 0) {
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
    };
    propagateNumericAdd();

    // Vertex-batcher specialization (`packQuadAttributes`): two write-only
    // receivers, one fed float arithmetic and no bitwise ops, the other fed
    // bitwise integer packing and no float arithmetic, specialize directly to
    // Float32Array + Uint32Array so stores lower via ElemSetTyped.
    plan.receiverKinds.assign(plan.receivers.size(), il::kTypedArrayWordView);
    plan.provenInBoundsRead.assign(plan.receivers.size(), 0);
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
                plan.versionKinds = {plan.receiverKinds};
            } else if (floatStores[1] > 0 && bitwiseStores[1] == 0 && bitwiseStores[0] > 0 &&
                       floatStores[0] == 0) {
                concreteKinds = true;
                plan.receiverKinds[1] = BRONZE_ABI_TA_KIND_FLOAT32;
                plan.receiverKinds[0] = BRONZE_ABI_TA_KIND_UINT32;
                plan.versionKinds = {plan.receiverKinds};
            }
        }
    }

    // Multi-view float read+write specialization (`Quaternion.slerpFlat`,
    // `multiplyQuaternionsFlat`): when 1..3 receivers perform float math with
    // no integer bitwise packing and every read addresses `base + k` (`0 <= k
    // <= 1024`) ahead of any collection point, version directly for concrete
    // Float32Array / Float64Array tuples and guard read bounds at entry so
    // reads and writes lower via ElemGetTyped / ElemSetTyped without per-access
    // 4-way word-view kind switches or NaN-boxing.
    if (!concreteKinds && hasStores && plan.receivers.size() >= 1 && plan.receivers.size() <= 3) {
        constexpr int32_t kMaxProbeOffset = 1024;
        constexpr size_t kMaxReadProbes = 4;
        const auto extractConstOffset = [&](il::ValueId v, int32_t& outK) -> bool {
            const il::Instruction* d = defInst(v);
            if (d && d->op == il::Op::Box &&
                (d->boxType == il::Type::F64 || d->boxType == il::Type::I32) &&
                d->operands.size() == 1) {
                d = defInst(d->operands[0]);
            }
            if (!d) return false;
            if (d->op == il::Op::ConstI32 && d->immI32 >= 0 && d->immI32 <= kMaxProbeOffset) {
                outK = d->immI32;
                return true;
            }
            if (d->op == il::Op::ConstF64 && std::isfinite(d->immF64) && d->immF64 >= 0.0 &&
                d->immF64 <= static_cast<double>(kMaxProbeOffset) &&
                std::floor(d->immF64) == d->immF64) {
                outK = static_cast<int32_t>(d->immF64);
                return true;
            }
            return false;
        };
        const auto resolvePreBodyBase = [&](il::ValueId v, il::ValueId& outBase) -> bool {
            const il::Instruction* d = defInst(v);
            if (d && d->op == il::Op::Box &&
                (d->boxType == il::Type::F64 || d->boxType == il::Type::I32) &&
                d->operands.size() == 1 && !definedIn(d->operands[0])) {
                v = d->operands[0];
            } else if (d && d->op == il::Op::Unbox && d->type == il::Type::F64 &&
                       d->operands.size() == 1 && !definedIn(d->operands[0])) {
                v = d->operands[0];
            }
            if (definedIn(v) || v >= plan.types.size()) return false;
            if (plan.types[v] == il::Type::F64 || plan.types[v] == il::Type::I32 ||
                isBoxedNumber(v) || rawUnboxAt(v, entry, 0) != il::kNoValue ||
                canGuardPreBody(v)) {
                outBase = v;
                return true;
            }
            return false;
        };
        const auto extractReadIndex = [&](il::ValueId idxVal, il::ValueId& outBase,
                                          int32_t& outOff) -> bool {
            if (extractConstOffset(idxVal, outOff)) {
                outBase = il::kNoValue;
                return true;
            }
            il::ValueId u = idxVal;
            const il::Instruction* d = defInst(u);
            if (d && d->op == il::Op::Box &&
                (d->boxType == il::Type::F64 || d->boxType == il::Type::I32) &&
                d->operands.size() == 1) {
                u = d->operands[0];
                d = defInst(u);
            }
            if (d && d->op == il::Op::Add && d->operands.size() == 2 &&
                (d->type == il::Type::F64 || d->type == il::Type::I32 ||
                 plan.numericAdd.count(d->result) != 0)) {
                for (int side = 0; side < 2; ++side) {
                    int32_t k = 0;
                    il::ValueId base = il::kNoValue;
                    if (extractConstOffset(d->operands[1 - side], k) &&
                        resolvePreBodyBase(d->operands[side], base)) {
                        outBase = base;
                        outOff = k;
                        return true;
                    }
                }
            }
            if (resolvePreBodyBase(u, outBase)) {
                outOff = 0;
                return true;
            }
            return false;
        };
        const auto willFastCopyCollect = [&](const il::Instruction& inst, il::BlockId b,
                                             uint32_t i) -> bool {
            if (!il::canCollect(inst) || isFlaggedAccess(inst)) return false;
            if (inst.op == il::Op::Unbox && inst.type == il::Type::F64 &&
                inst.operands.size() == 1) {
                const il::ValueId op0 = inst.operands[0];
                return !(isNumericProducer(op0, b, i) || canGuardPreBody(op0));
            }
            if (isBodyPromotableDynOp(fn, plan, inst)) {
                for (il::ValueId opId : inst.operands) {
                    if (!(isNumericProducer(opId, b, i) || canGuardPreBody(opId))) return true;
                }
                return false;
            }
            return true;
        };

        const auto isIndexOnlyAdd = [&](il::ValueId addRes) -> bool {
            auto onlyIndexUses = [&](il::ValueId val, bool allowBox) -> bool {
                bool any = false;
                for (il::BlockId ub : plan.body) {
                    for (const auto& u : fn.blocks[ub].instructions) {
                        for (size_t oi = 0; oi < u.operands.size(); ++oi) {
                            if (u.operands[oi] != val) continue;
                            any = true;
                            if (isFlaggedAccess(u) && oi == 1) continue;
                            if (allowBox && u.op == il::Op::Box && u.boxType == il::Type::F64 &&
                                u.result != il::kNoValue) {
                                for (il::BlockId bb : plan.body) {
                                    for (const auto& bu : fn.blocks[bb].instructions) {
                                        for (size_t boi = 0; boi < bu.operands.size(); ++boi) {
                                            if (bu.operands[boi] == u.result &&
                                                !(isFlaggedAccess(bu) && boi == 1)) {
                                                return false;
                                            }
                                        }
                                        for (il::ValueId a : bu.target.args) {
                                            if (a == u.result) return false;
                                        }
                                        for (il::ValueId a : bu.elseTarget.args) {
                                            if (a == u.result) return false;
                                        }
                                    }
                                }
                                continue;
                            }
                            return false;
                        }
                        for (il::ValueId a : u.target.args) {
                            if (a == val) return false;
                        }
                        for (il::ValueId a : u.elseTarget.args) {
                            if (a == val) return false;
                        }
                    }
                }
                return any;
            };
            return onlyIndexUses(addRes, /*allowBox=*/true);
        };

        bool eligible = true;
        bool hasFloatWork = false;
        bool seenCollect = false;
        std::vector<uint8_t> readsRecv(plan.receivers.size(), 0);
        std::vector<uint8_t> writesRecv(plan.receivers.size(), 0);
        std::map<std::pair<size_t, il::ValueId>, int32_t> probeMax;

        for (il::BlockId b : plan.body) {
            const auto& insts = fn.blocks[b].instructions;
            for (uint32_t i = 0; i < insts.size() && eligible; ++i) {
                const il::Instruction& inst = insts[i];
                if (isBodyBitwiseBinOp(inst.op) || inst.op == il::Op::BitNot ||
                    inst.op == il::Op::MathImul || inst.op == il::Op::ToInt32) {
                    eligible = false;
                    break;
                }
                if (isBodyFloatArithOp(inst.op) || inst.op == il::Op::MathUnary ||
                    (inst.op == il::Op::Add &&
                     (inst.type == il::Type::F64 || plan.numericAdd.count(inst.result) != 0) &&
                     !isIndexOnlyAdd(inst.result))) {
                    hasFloatWork = true;
                }
                if (isFlaggedAccess(inst)) {
                    const size_t rIdx = receiverIndex(inst.operands[0]);
                    for (size_t oi = 1; oi < inst.operands.size(); ++oi) {
                        if (isNonNumberValue(inst.operands[oi])) eligible = false;
                    }
                    if (inst.op == il::Op::ElemGet) {
                        readsRecv[rIdx] = 1;
                        il::ValueId base = il::kNoValue;
                        int32_t off = 0;
                        if (seenCollect || !extractReadIndex(inst.operands[1], base, off)) {
                            eligible = false;
                            break;
                        }
                        auto [it, inserted] = probeMax.emplace(std::make_pair(rIdx, base), off);
                        if (!inserted && off > it->second) it->second = off;
                        if (probeMax.size() > kMaxReadProbes) {
                            eligible = false;
                            break;
                        }
                    } else {
                        writesRecv[rIdx] = 1;
                    }
                }
                if (willFastCopyCollect(inst, b, i)) seenCollect = true;
            }
        }

        if (eligible && hasFloatWork) {
            concreteKinds = true;
            for (size_t r = 0; r < plan.receivers.size(); ++r) {
                plan.provenInBoundsRead[r] = readsRecv[r];
            }
            for (const auto& [key, maxOff] : probeMax) {
                plan.readProbes.push_back(TypedBodyReadProbe{key.first, key.second, maxOff});
                if (key.second != il::kNoValue && canGuardPreBody(key.second)) {
                    plan.guardedPreBody.push_back(key.second);
                }
            }
            propagateNumericAdd();

            const size_t nRecv = plan.receivers.size();
            size_t writeOnlyCount = 0;
            size_t readOnlyCount = 0;
            size_t writeOnlyIdx = 0;
            for (size_t r = 0; r < nRecv; ++r) {
                if (writesRecv[r] && !readsRecv[r]) {
                    ++writeOnlyCount;
                    writeOnlyIdx = r;
                } else if (readsRecv[r] && !writesRecv[r]) {
                    ++readOnlyCount;
                }
            }
            std::vector<int32_t> allF32(nRecv, BRONZE_ABI_TA_KIND_FLOAT32);
            std::vector<int32_t> allF64(nRecv, BRONZE_ABI_TA_KIND_FLOAT64);
            plan.versionKinds.push_back(allF32);
            if (nRecv >= 2 && writeOnlyCount == 1 && writeOnlyCount + readOnlyCount == nRecv) {
                std::vector<int32_t> f64OutF32In = allF32;
                f64OutF32In[writeOnlyIdx] = BRONZE_ABI_TA_KIND_FLOAT64;
                plan.versionKinds.push_back(std::move(f64OutF32In));
            }
            plan.versionKinds.push_back(allF64);
            plan.receiverKinds = plan.versionKinds.front();
        }
    }

    // Pre-body Dynamic values (`index`, `textureId`, `dstOffset`, `t`) used as
    // numeric operands inside `body` are guarded once in `enterThrough`.
    for (il::BlockId b : plan.body) {
        const auto& insts = fn.blocks[b].instructions;
        for (uint32_t i = 0; i < insts.size(); ++i) {
            const il::Instruction& inst = insts[i];
            const bool relNumeric =
                isBodyRelOp(inst.op) && inst.operands.size() == 2 &&
                !isNonNumberValue(inst.operands[0]) && !isNonNumberValue(inst.operands[1]) &&
                (isNumericProducer(inst.operands[0], b, i) ||
                 isNumericProducer(inst.operands[1], b, i));
            const bool prom = isBodyPromotableDynOp(fn, plan, inst) ||
                              (inst.op == il::Op::Unbox && !inst.rawUnbox && !inst.nullishUnbox &&
                               inst.type == il::Type::F64) ||
                              inst.op == il::Op::ToInt32 || relNumeric ||
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
        const il::Instruction& def = fn.blocks[defBlk].instructions[defIdx];
        if (isProvenFloatRead(def)) return false;
        return !isBodyPromotableDynOp(fn, plan, def);
    };
    for (il::BlockId b : plan.body) {
        for (uint32_t i = 0; i < fn.blocks[b].instructions.size(); ++i) {
            const il::Instruction& inst = fn.blocks[b].instructions[i];
            const bool relNumeric =
                isBodyRelOp(inst.op) && inst.operands.size() == 2 &&
                !isNonNumberValue(inst.operands[0]) && !isNonNumberValue(inst.operands[1]) &&
                (isNumericProducer(inst.operands[0], b, i) ||
                 isNumericProducer(inst.operands[1], b, i));
            const bool numericOp =
                isBodyPromotableDynOp(fn, plan, inst) || inst.op == il::Op::ToInt32 || relNumeric ||
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
                for (size_t oi = 1; oi < inst.operands.size(); ++oi) {
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
    // The split at `index` of block `b`, made if there is none; the block's
    // splits stay in instruction order.
    const auto splitAt = [&](il::BlockId b, uint32_t index) -> TypedBodyGuardSplit& {
        auto& splits = plan.splitsOf[b];
        auto it = std::lower_bound(splits.begin(), splits.end(), index,
                                   [](const TypedBodyGuardSplit& sp, uint32_t at) { return sp.index < at; });
        if (it == splits.end() || it->index != index) {
            TypedBodyGuardSplit sp;
            sp.index = index;
            it = splits.insert(it, std::move(sp));
            ++plan.totalSplits;
        }
        return *it;
    };
    // A named read off a boxed receiver, which a split can stand in front of
    // (typed_body_version.h, "READS").
    const auto exitableRead = [&](const il::Instruction& inst) {
        return inst.op == il::Op::PropGet && inst.result != il::kNoValue && inst.operands.size() == 1 &&
               !inst.shapeGuarded && inst.directTarget == il::Instruction::kNoDirectTarget &&
               inst.operands[0] < plan.types.size() && plan.types[inst.operands[0]] == il::Type::Dynamic;
    };
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
            if (concreteKinds && exitableRead(inst) && canSplit[i] && plan.totalSplits < kMaxGuardSplits) {
                splitAt(b, i).shapeOf = inst.operands[0];
            }
            bool needSplit = false;
            if (canSplit[i] && plan.totalSplits < kMaxGuardSplits) {
                const bool relNumeric =
                    isBodyRelOp(inst.op) && inst.operands.size() == 2 &&
                    !isNonNumberValue(inst.operands[0]) && !isNonNumberValue(inst.operands[1]) &&
                    (isNumericProducer(inst.operands[0], b, i) ||
                     isNumericProducer(inst.operands[1], b, i));
                const bool checksOps =
                    isBodyPromotableDynOp(fn, plan, inst) || inst.op == il::Op::ToInt32 ||
                    relNumeric || (concreteKinds && isFlaggedAccess(inst)) ||
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
                std::vector<il::ValueId> fresh;
                for (il::ValueId v : avail) {
                    if (guardedHere.insert(v).second) fresh.push_back(v);
                }
                if (!fresh.empty()) splitAt(b, i).guarded = std::move(fresh);
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
                    auto& guarded = splitAt(b, splitIdx).guarded;
                    guarded.insert(guarded.end(), rem.begin(), rem.end());
                }
            }
        }
    }
    return true;
}

}  // namespace

void resolveReadExits(il::Module& module) {
    for (il::Function& fn : module.functions) {
        std::vector<uint32_t> preds;
        bool folded = false;
        for (size_t x = 0; x < fn.blocks.size(); ++x) {
            auto& insts = fn.blocks[x].instructions;
            if (insts.size() < 2) continue;
            il::Instruction& test = insts[insts.size() - 2];
            il::Instruction& term = insts.back();
            if (test.op != il::Op::IsShape || test.staticSlot != il::Instruction::kNoStaticSlot) continue;
            if (preds.empty()) {
                preds.assign(fn.blocks.size(), 0);
                for (const il::Block& blk : fn.blocks) {
                    if (blk.instructions.empty()) continue;
                    const il::Instruction& t = blk.instructions.back();
                    for (const il::BlockTarget* e : {&t.target, &t.elseTarget}) {
                        if (e->block != il::kNoBlock && e->block < preds.size()) ++preds[e->block];
                    }
                    if (blk.handler != il::kNoBlock && blk.handler < preds.size()) ++preds[blk.handler];
                }
            }
            il::Instruction* read = nullptr;
            if (term.op == il::Op::Branch && term.operands.size() == 1 && term.operands[0] == test.result &&
                term.target.block < fn.blocks.size() && term.target.block != term.elseTarget.block &&
                preds[term.target.block] == 1) {
                for (il::Instruction& inst : fn.blocks[term.target.block].instructions) {
                    if (inst.op == il::Op::PropGet && inst.operands.size() == 1 &&
                        inst.operands[0] == test.operands[0]) {
                        if (inst.result != il::kNoValue && !inst.shapeGuarded &&
                            inst.staticSlot != il::Instruction::kNoStaticSlot &&
                            inst.directTarget == il::Instruction::kNoDirectTarget) {
                            read = &inst;
                        }
                        break;
                    }
                    if (il::isTerminator(inst.op) || il::canCollect(inst) || il::canThrow(inst)) break;
                }
            }
            // A cell claim is filled by a site's own miss, which a bare load
            // never runs: the test asks the cell of the read a miss leaves
            // for, the original body's, which claims the same key at the same
            // slot and fills it there. A family claim has no cell.
            uint32_t cell = 0;
            if (read != nullptr && read->familyLo == il::Instruction::kNoFamily) {
                const il::Block* tail =
                    term.elseTarget.block < fn.blocks.size() ? &fn.blocks[term.elseTarget.block] : nullptr;
                const il::Instruction* slow =
                    tail != nullptr && !tail->instructions.empty() ? &tail->instructions.front() : nullptr;
                if (slow != nullptr && slow->op == il::Op::PropGet && slow->operands.size() == 1 &&
                    !slow->shapeGuarded && slow->keyIndex == read->keyIndex &&
                    slow->staticSlot == read->staticSlot && slow->familyLo == il::Instruction::kNoFamily &&
                    slow->directTarget == il::Instruction::kNoDirectTarget) {
                    cell = slow->staticCellIndex;
                } else {
                    read = nullptr;
                }
            }
            if (read != nullptr) {
                test.staticSlot = read->staticSlot;
                test.staticCellIndex = cell;
                test.familyLo = read->familyLo;
                test.familySpan = read->familySpan;
                read->shapeGuarded = true;
                continue;
            }
            test.op = il::Op::ConstBool;
            test.operands.clear();
            test.immI32 = 1;
            if (term.op == il::Op::Branch && term.operands.size() == 1 && term.operands[0] == test.result) {
                term.op = il::Op::Jump;
                term.operands.clear();
                term.elseTarget = {};
                folded = true;
            }
        }
        if (folded) {
            std::vector<uint8_t> unused(fn.blocks.size(), 0);
            pruneUnreachable(fn, unused);
        }
    }
}

namespace {

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
