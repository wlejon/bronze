#include "check/prove_equiv_heap.h"

#include <cstdlib>
#include <set>
#include <unordered_set>

namespace bronze::check {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;

namespace {

bool isLocalRootPath(const std::string& path) {
    return path.rfind("__local_", 0) == 0;
}

bool parseSymbolicIdxPath(const std::string& path, std::string& outBase, BVNodeId& outIdxNode) {
    auto lb = path.rfind("[@");
    if (lb == std::string::npos || path.back() != ']') return false;
    std::string numStr = path.substr(lb + 2, path.size() - (lb + 3));
    if (numStr.empty()) return false;
    char* endPtr = nullptr;
    unsigned long val = std::strtoul(numStr.c_str(), &endPtr, 10);
    if (!endPtr || *endPtr != '\0') return false;
    outBase = path.substr(0, lb);
    outIdxNode = static_cast<BVNodeId>(val);
    return true;
}

bool parseConstElemPath(const std::string& path, std::string& outBase, int64_t& outIdx) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) return false;
    std::string key = path.substr(dot + 1);
    if (key.empty() || (key[0] != '-' && (key[0] < '0' || key[0] > '9'))) return false;
    char* endPtr = nullptr;
    long long val = std::strtoll(key.c_str(), &endPtr, 10);
    if (!endPtr || *endPtr != '\0') return false;
    outBase = path.substr(0, dot);
    outIdx = static_cast<int64_t>(val);
    return true;
}

}  // namespace

BVNodeId SymbolicHeapTable::getInitialVal(const std::string& path) {
    auto it = initialVals_.find(path);
    if (it != initialVals_.end()) return it->second;
    BVNodeId raw32 = ctx_.make_var("heap:" + path, 32);
    BVNodeId sext64 = ctx_.make_sext(raw32, 32);
    initialVals_[path] = sext64;
    varPaths_.push_back(path);
    varNodes_.push_back(sext64);
    return sext64;
}

BVNodeId SymbolicHeapTable::getObjectToken(const std::string& rootPath) {
    auto it = objectTokens_.find(rootPath);
    if (it != objectTokens_.end()) return it->second;
    uint64_t tokenImm = 0x00B1000000000001ULL + static_cast<uint64_t>(objectTokens_.size());
    BVNodeId tok = ctx_.make_const(tokenImm, 64);
    objectTokens_[rootPath] = tok;
    return tok;
}

void SymbolicHeapTable::recordAliasedRoots(const std::string& rootA,
                                           const std::string& rootB,
                                           BVNodeId eqCond) {
    if (rootA.empty() || rootB.empty() || rootA == rootB || eqCond == 0) return;
    std::string a = (rootA < rootB) ? rootA : rootB;
    std::string b = (rootA < rootB) ? rootB : rootA;
    for (const auto& existing : aliasedPairs_) {
        if (existing.rootA == a && existing.rootB == b && existing.eqCond == eqCond) return;
    }
    aliasedPairs_.push_back({std::move(a), std::move(b), eqCond});
}

BVNodeId SymbolicHeapTable::buildAliasCongruenceCond() {
    if (aliasedPairs_.empty()) return 0;
    BVNodeId conj = 0;
    auto andCond = [&](BVNodeId c) {
        if (c == 0) return;
        conj = (conj == 0) ? c : ctx_.make_binary(BVOp::And, conj, c);
    };
    std::vector<std::string> snapshot = varPaths_;
    for (const auto& pair : aliasedPairs_) {
        std::set<std::string> suffixes;
        std::string prefA = pair.rootA + ".";
        std::string prefB = pair.rootB + ".";
        for (const std::string& p : snapshot) {
            if (p.rfind(prefA, 0) == 0) {
                suffixes.insert(p.substr(prefA.size()));
            } else if (p.rfind(prefB, 0) == 0) {
                suffixes.insert(p.substr(prefB.size()));
            }
        }
        BVNodeId notEq = ctx_.make_unary(BVOp::Not, pair.eqCond);
        for (const std::string& sfx : suffixes) {
            BVNodeId va = getInitialVal(prefA + sfx);
            BVNodeId vb = getInitialVal(prefB + sfx);
            BVNodeId sameVal = ctx_.make_binary(BVOp::Eq, va, vb);
            andCond(ctx_.make_binary(BVOp::Or, notEq, sameVal));
        }
    }
    return conj;
}

void SymbolicHeapState::initFunctionHeap(SymbolicHeapTable& table,
                                         const il::Function& fn,
                                         std::unordered_map<il::ValueId, BVNodeId>& valMap,
                                         const std::string& thisPath,
                                         const std::vector<std::string>* callerArgPaths) {
    if (fn.needsThis && !fn.params.empty()) {
        objPaths[0] = thisPath;
        valMap[0] = table.getObjectToken(thisPath);
    }
    uint32_t firstSrc = static_cast<uint32_t>(fn.firstSourceParam());
    uint32_t srcCount = static_cast<uint32_t>(fn.callerParamCount());
    for (uint32_t i = 0; i < srcCount; ++i) {
        il::ValueId pid = static_cast<il::ValueId>(firstSrc + i);
        if (callerArgPaths && i < callerArgPaths->size() && !(*callerArgPaths)[i].empty()) {
            objPaths[pid] = (*callerArgPaths)[i];
        } else {
            objPaths[pid] = "arg_" + std::to_string(i);
        }
    }
}

std::string SymbolicHeapState::lookupObjPath(il::ValueId id) const {
    auto it = objPaths.find(id);
    return (it != objPaths.end()) ? it->second : std::string{};
}

void SymbolicHeapState::propagateAlias(il::ValueId src, il::ValueId dst) {
    auto it = objPaths.find(src);
    if (it != objPaths.end()) {
        objPaths[dst] = it->second;
    }
    auto cmpIt = objComparisons.find(src);
    if (cmpIt != objComparisons.end()) {
        objComparisons[dst] = cmpIt->second;
    }
    auto divIt = divResults.find(src);
    if (divIt != divResults.end()) {
        divResults[dst] = divIt->second;
    }
}

void SymbolicHeapState::recordObjComparison(il::ValueId resultId,
                                            il::ValueId lhsId,
                                            il::ValueId rhsId,
                                            bool isEq) {
    if (resultId == il::kNoValue) return;
    std::string lp = lookupObjPath(lhsId);
    std::string rp = lookupObjPath(rhsId);
    if (!lp.empty() && !rp.empty() && lp != rp) {
        objComparisons[resultId] = {std::move(lp), std::move(rp), isEq};
    }
}

void SymbolicHeapState::unifyBranchAlias(SymbolicHeapTable& table,
                                         il::ValueId condValId,
                                         BVNodeId condBool1,
                                         bool isTrueBranch) {
    auto it = objComparisons.find(condValId);
    if (it == objComparisons.end()) return;
    const auto& info = it->second;
    bool aliasHoldsOnBranch = (info.isEq == isTrueBranch);
    if (!aliasHoldsOnBranch) return;

    std::string canonical = (info.lhsPath < info.rhsPath) ? info.lhsPath : info.rhsPath;
    std::string replaced = (info.lhsPath < info.rhsPath) ? info.rhsPath : info.lhsPath;
    BVNodeId eqCond = info.isEq ? condBool1 : table.ctx().make_unary(BVOp::Not, condBool1);
    table.recordAliasedRoots(canonical, replaced, eqCond);

    auto rewritePath = [&](std::string& p) {
        if (p == replaced) {
            p = canonical;
        } else if (p.rfind(replaced + ".", 0) == 0) {
            p = canonical + p.substr(replaced.size());
        } else if (p.rfind(replaced + "[", 0) == 0) {
            p = canonical + p.substr(replaced.size());
        }
    };
    for (auto& [_, p] : objPaths) rewritePath(p);
    for (auto& [_, p] : envObjPaths) rewritePath(p);

    std::map<std::string, BVNodeId> nextWrites;
    for (const auto& [k, v] : writes) {
        std::string nk = k;
        rewritePath(nk);
        nextWrites[nk] = v;
    }
    writes = std::move(nextWrites);
}

void SymbolicHeapState::propagateEnvSet(uint64_t envKey, il::ValueId src) {
    auto it = objPaths.find(src);
    if (it != objPaths.end()) {
        envObjPaths[envKey] = it->second;
    }
}

void SymbolicHeapState::propagateEnvGet(uint64_t envKey, il::ValueId dst) {
    auto it = envObjPaths.find(envKey);
    if (it != envObjPaths.end()) {
        objPaths[dst] = it->second;
    }
}

BVNodeId SymbolicHeapState::createLocalObject(SymbolicHeapTable& table, il::ValueId resultId) {
    std::string path = "__local_" + std::to_string(localObjCounter++);
    if (resultId != il::kNoValue) {
        objPaths[resultId] = path;
    }
    return table.getObjectToken(path);
}

bool SymbolicHeapState::evalPropGet(SymbolicHeapTable& table,
                                    il::ValueId recvId,
                                    const std::string& key,
                                    il::ValueId resultId,
                                    BVNodeId& outVal) {
    auto it = objPaths.find(recvId);
    if (it == objPaths.end() || key.empty()) return false;
    std::string path = it->second + "." + key;
    if (resultId != il::kNoValue) {
        objPaths[resultId] = path;
    }
    auto wIt = writes.find(path);
    if (wIt != writes.end()) {
        outVal = wIt->second;
        return true;
    }
    // Check if `it->second` has symbolic element writes and `key` is a constant index.
    int64_t constIdx = 0;
    std::string base;
    if (parseConstElemPath(path, base, constIdx)) {
        BVContext& ctx = table.ctx();
        BVNodeId idxConst = ctx.make_const(static_cast<uint64_t>(constIdx), 64);
        BVNodeId res = table.getInitialVal(path);
        for (const auto& [wPath, wVal] : writes) {
            std::string symBase;
            BVNodeId symIdx = 0;
            if (parseSymbolicIdxPath(wPath, symBase, symIdx) && symBase == base) {
                BVNodeId eq = ctx.make_binary(BVOp::Eq, idxConst, symIdx);
                res = ctx.make_ite(eq, wVal, res);
            }
        }
        outVal = res;
        return true;
    }
    outVal = table.getInitialVal(path);
    return true;
}

bool SymbolicHeapState::evalPropSet(SymbolicHeapTable& table,
                                    il::ValueId recvId,
                                    const std::string& key,
                                    BVNodeId valNode,
                                    il::ValueId valId) {
    (void)table;
    auto it = objPaths.find(recvId);
    if (it == objPaths.end() || key.empty()) return false;
    std::string path = it->second + "." + key;
    writes[path] = valNode;
    if (valId != il::kNoValue) {
        auto vObj = objPaths.find(valId);
        if (vObj != objPaths.end()) {
            // If an object/array is stored into a property, alias `path` to it
            for (const auto& [k, v] : writes) {
                if (k.rfind(vObj->second + ".", 0) == 0) {
                    writes[path + k.substr(vObj->second.size())] = v;
                }
            }
        }
    }
    return true;
}

bool SymbolicHeapState::evalElemGet(SymbolicHeapTable& table,
                                    il::ValueId recvId,
                                    BVNodeId idxNode,
                                    il::ValueId resultId,
                                    BVNodeId& outVal) {
    BVContext& ctx = table.ctx();
    if (idxNode < ctx.nodes().size() && ctx.nodes()[idxNode].op == BVOp::Const) {
        int64_t imm = static_cast<int64_t>(ctx.nodes()[idxNode].imm);
        return evalPropGet(table, recvId, std::to_string(imm), resultId, outVal);
    }
    auto it = objPaths.find(recvId);
    if (it == objPaths.end()) return false;
    const std::string& base = it->second;
    std::string symPath = base + "[@" + std::to_string(idxNode) + "]";
    if (resultId != il::kNoValue) {
        objPaths[resultId] = symPath;
    }
    auto directIt = writes.find(symPath);
    if (directIt != writes.end()) {
        outVal = directIt->second;
        return true;
    }
    BVNodeId res = table.getInitialVal(symPath);
    for (const auto& [wPath, wVal] : writes) {
        std::string wBase;
        BVNodeId wSymIdx = 0;
        int64_t wConstIdx = 0;
        if (parseSymbolicIdxPath(wPath, wBase, wSymIdx) && wBase == base) {
            BVNodeId eq = ctx.make_binary(BVOp::Eq, idxNode, wSymIdx);
            res = ctx.make_ite(eq, wVal, res);
        } else if (parseConstElemPath(wPath, wBase, wConstIdx) && wBase == base) {
            BVNodeId cNode = ctx.make_const(static_cast<uint64_t>(wConstIdx), 64);
            BVNodeId eq = ctx.make_binary(BVOp::Eq, idxNode, cNode);
            res = ctx.make_ite(eq, wVal, res);
        }
    }
    outVal = res;
    return true;
}

bool SymbolicHeapState::evalElemSet(SymbolicHeapTable& table,
                                    il::ValueId recvId,
                                    BVNodeId idxNode,
                                    BVNodeId valNode,
                                    il::ValueId valId) {
    BVContext& ctx = table.ctx();
    if (idxNode < ctx.nodes().size() && ctx.nodes()[idxNode].op == BVOp::Const) {
        int64_t imm = static_cast<int64_t>(ctx.nodes()[idxNode].imm);
        return evalPropSet(table, recvId, std::to_string(imm), valNode, valId);
    }
    auto it = objPaths.find(recvId);
    if (it == objPaths.end()) return false;
    std::string symPath = it->second + "[@" + std::to_string(idxNode) + "]";
    writes[symPath] = valNode;
    return true;
}

BVNodeId SymbolicHeapState::getFinalValue(SymbolicHeapTable& table,
                                          const std::string& path) const {
    auto it = writes.find(path);
    if (it != writes.end()) return it->second;
    return table.getInitialVal(path);
}

SymbolicHeapState SymbolicHeapState::mergeBranches(SymbolicHeapTable& table,
                                                   BVNodeId condNode,
                                                   const SymbolicHeapState& tHeap,
                                                   const SymbolicHeapState& fHeap) {
    BVContext& ctx = table.ctx();
    SymbolicHeapState merged = tHeap;
    for (const auto& [k, v] : fHeap.objPaths) {
        merged.objPaths.try_emplace(k, v);
    }
    for (const auto& [k, v] : fHeap.envObjPaths) {
        merged.envObjPaths.try_emplace(k, v);
    }
    merged.localObjCounter = std::max(tHeap.localObjCounter, fHeap.localObjCounter);

    std::set<std::string> allPaths;
    for (const auto& [p, _] : tHeap.writes) allPaths.insert(p);
    for (const auto& [p, _] : fHeap.writes) allPaths.insert(p);

    merged.writes.clear();
    for (const std::string& p : allPaths) {
        BVNodeId tv = tHeap.getFinalValue(table, p);
        BVNodeId fv = fHeap.getFinalValue(table, p);
        merged.writes[p] = (tv == fv) ? tv : ctx.make_ite(condNode, tv, fv);
    }
    return merged;
}

BVNodeId SymbolicHeapState::buildHeapMismatchCond(SymbolicHeapTable& table,
                                                  const SymbolicHeapState& oldHeap,
                                                  const SymbolicHeapState& newHeap,
                                                  std::vector<std::string>* outCheckedPaths) {
    BVContext& ctx = table.ctx();
    BVNodeId mismatch = 0;
    auto orCond = [&](BVNodeId c) {
        if (c == 0) return;
        mismatch = (mismatch == 0) ? c : ctx.make_binary(BVOp::Or, mismatch, c);
    };

    // Group symbolic-index writes by base object so `dst[i * 4] = v` vs `dst[i << 2] = v`
    // can be proven equivalent when both the index expression and stored value match.
    struct SymWrite {
        std::string fullPath;
        BVNodeId idxNode;
        BVNodeId valNode;
    };
    std::map<std::string, std::vector<SymWrite>> oldSymByBase;
    std::map<std::string, std::vector<SymWrite>> newSymByBase;
    std::set<std::string> namedPaths;

    for (const auto& [p, v] : oldHeap.writes) {
        if (isLocalRootPath(p)) continue;
        std::string base;
        BVNodeId idx = 0;
        if (parseSymbolicIdxPath(p, base, idx)) {
            oldSymByBase[base].push_back({p, idx, v});
        } else {
            namedPaths.insert(p);
        }
    }
    for (const auto& [p, v] : newHeap.writes) {
        if (isLocalRootPath(p)) continue;
        std::string base;
        BVNodeId idx = 0;
        if (parseSymbolicIdxPath(p, base, idx)) {
            newSymByBase[base].push_back({p, idx, v});
        } else {
            namedPaths.insert(p);
        }
    }

    for (const std::string& p : namedPaths) {
        if (outCheckedPaths) outCheckedPaths->push_back(p);
        BVNodeId oldV = oldHeap.getFinalValue(table, p);
        BVNodeId newV = newHeap.getFinalValue(table, p);
        if (oldV != newV) {
            orCond(ctx.make_binary(BVOp::Ne, oldV, newV));
        }
    }

    std::set<std::string> symBases;
    for (const auto& [b, _] : oldSymByBase) symBases.insert(b);
    for (const auto& [b, _] : newSymByBase) symBases.insert(b);

    for (const std::string& b : symBases) {
        const auto& ow = oldSymByBase[b];
        const auto& nw = newSymByBase[b];
        if (ow.size() == nw.size()) {
            for (size_t i = 0; i < ow.size(); ++i) {
                if (outCheckedPaths) outCheckedPaths->push_back(ow[i].fullPath);
                if (ow[i].idxNode != nw[i].idxNode) {
                    orCond(ctx.make_binary(BVOp::Ne, ow[i].idxNode, nw[i].idxNode));
                }
                if (ow[i].valNode != nw[i].valNode) {
                    orCond(ctx.make_binary(BVOp::Ne, ow[i].valNode, nw[i].valNode));
                }
            }
        } else {
            for (const auto& w : ow) {
                if (outCheckedPaths) outCheckedPaths->push_back(w.fullPath);
                BVNodeId nv = newHeap.getFinalValue(table, w.fullPath);
                if (w.valNode != nv) orCond(ctx.make_binary(BVOp::Ne, w.valNode, nv));
            }
            for (const auto& w : nw) {
                if (outCheckedPaths) outCheckedPaths->push_back(w.fullPath);
                BVNodeId ov = oldHeap.getFinalValue(table, w.fullPath);
                if (ov != w.valNode) orCond(ctx.make_binary(BVOp::Ne, ov, w.valNode));
            }
        }
    }

    return mismatch;
}

namespace {

enum class DivUseKind : uint8_t { TruncatedToInt, FloorOrCeilOnly, RawFloat };

DivUseKind classifyDivResultUsage(const il::Module& mod, const il::Block& blk, size_t divInstIdx) {
    il::ValueId rootRes = blk.instructions[divInstIdx].result;
    if (rootRes == il::kNoValue) return DivUseKind::TruncatedToInt;
    std::unordered_set<il::ValueId> tracked{rootRes};
    bool sawFloorOrCeil = false;
    bool sawTrunc = false;

    for (size_t j = divInstIdx + 1; j < blk.instructions.size(); ++j) {
        const auto& u = blk.instructions[j];
        bool usesTracked = false;
        for (il::ValueId op : u.operands) {
            if (tracked.count(op)) {
                usesTracked = true;
                break;
            }
        }
        if (!usesTracked) continue;

        using Op = il::Op;
        if (u.op == Op::Box || u.op == Op::Unbox || u.op == Op::ToNumeric) {
            if (u.result != il::kNoValue) tracked.insert(u.result);
            continue;
        }
        if (u.op == Op::ToInt32 || u.op == Op::BitOr || u.op == Op::BitAnd ||
            u.op == Op::BitXor || u.op == Op::Shl || u.op == Op::Shr || u.op == Op::UShr) {
            sawTrunc = true;
            continue;
        }
        if (u.op == Op::MathUnary) {
            auto kind = static_cast<il::MathUnaryFn>(u.immI32);
            if (kind == il::MathUnaryFn::Trunc) {
                sawTrunc = true;
                continue;
            }
            if (kind == il::MathUnaryFn::Floor || kind == il::MathUnaryFn::Ceil) {
                sawFloorOrCeil = true;
                continue;
            }
            return DivUseKind::RawFloat;
        }
        if (u.op == Op::MethodCall && u.keyIndex < mod.keyConstants.size() &&
            u.operands.size() == 2 && tracked.count(u.operands[1])) {
            const std::string& m = mod.keyConstants[u.keyIndex];
            if (m == "trunc") {
                sawTrunc = true;
                continue;
            }
            if (m == "floor" || m == "ceil") {
                sawFloorOrCeil = true;
                continue;
            }
        }
        return DivUseKind::RawFloat;
    }

    if (sawFloorOrCeil && !sawTrunc) return DivUseKind::FloorOrCeilOnly;
    if (sawTrunc && !sawFloorOrCeil) return DivUseKind::TruncatedToInt;
    return DivUseKind::RawFloat;
}

BVNodeId makeConstAwareDivOrRem(BVContext& ctx, BVOp op, BVNodeId a, BVNodeId b) {
    const auto& bNode = ctx.nodes()[b];
    if (bNode.op == BVOp::Const) {
        int64_t divisor = static_cast<int64_t>(bNode.imm);
        if (divisor > 0 && divisor <= INT32_MAX &&
            (static_cast<uint64_t>(divisor) & (static_cast<uint64_t>(divisor) - 1)) != 0) {
            BVNodeId a32 = ctx.make_extract(a, 31, 0);
            BVNodeId b32 = ctx.make_const(static_cast<uint64_t>(divisor), 32);
            BVNodeId r32 = brass::verify::expand_const_div_rem_bv(ctx, op, a32, b32, 32);
            return ctx.make_sext(r32, 32);
        }
    }
    return ctx.make_binary(op, a, b);
}

}  // namespace

bool evalDivOrModSymbolic(BVContext& ctx,
                          const il::Module& mod,
                          const il::Block& blk,
                          size_t instIdx,
                          BVNodeId a,
                          BVNodeId b,
                          SymbolicHeapState& heap,
                          BVNodeId& outVal,
                          std::string* outReason) {
    const auto& inst = blk.instructions[instIdx];
    const auto& aNode = ctx.nodes()[a];
    const auto& bNode = ctx.nodes()[b];

    if (bNode.op == BVOp::Const && static_cast<int64_t>(bNode.imm) == 0) {
        if (outReason) *outReason = "division by zero constant";
        return false;
    }

    if (inst.op == il::Op::Mod) {
        outVal = makeConstAwareDivOrRem(ctx, BVOp::SRem, a, b);
        return true;
    }

    DivUseKind useKind = classifyDivResultUsage(mod, blk, instIdx);
    if (useKind == DivUseKind::FloorOrCeilOnly && inst.result != il::kNoValue) {
        heap.divResults[inst.result] = {a, b};
    }

    if (useKind == DivUseKind::RawFloat) {
        if (bNode.op != BVOp::Const || static_cast<int64_t>(bNode.imm) <= 0) {
            if (outReason) *outReason = "non-integer floating-point division";
            return false;
        }
        int64_t divisor = static_cast<int64_t>(bNode.imm);
        if (aNode.op == BVOp::Const) {
            int64_t dividend = static_cast<int64_t>(aNode.imm);
            if (dividend % divisor != 0) {
                if (outReason) *outReason = "fractional floating-point division constant";
                return false;
            }
            outVal = ctx.make_const(static_cast<uint64_t>(dividend / divisor), 64);
            return true;
        }
        if (divisor > 1) {
            BVNodeId sremNode = makeConstAwareDivOrRem(ctx, BVOp::SRem, a, b);
            size_t nVars = ctx.vars().size();
            static constexpr int64_t kSamples[] = {1, 2, 3, 4, 5, -1, -2, -3};
            for (int64_t s : kSamples) {
                std::vector<uint64_t> sample(nVars, static_cast<uint64_t>(static_cast<uint32_t>(s)));
                if (ctx.eval_concrete(sremNode, sample) != 0) {
                    if (outReason) *outReason = "non-integer floating-point division";
                    return false;
                }
            }
        }
    }

    outVal = makeConstAwareDivOrRem(ctx, BVOp::SDiv, a, b);
    return true;
}

BVNodeId evalFloorOrCeilSymbolic(BVContext& ctx,
                                 const SymbolicHeapState& heap,
                                 il::ValueId argId,
                                 BVNodeId argVal,
                                 bool isCeil) {
    auto it = heap.divResults.find(argId);
    if (it == heap.divResults.end()) return argVal;

    BVNodeId a = it->second.num;
    BVNodeId b = it->second.den;
    BVNodeId zero64 = ctx.make_const(0, 64);
    BVNodeId one64 = ctx.make_const(1, 64);
    BVNodeId sdiv = makeConstAwareDivOrRem(ctx, BVOp::SDiv, a, b);
    BVNodeId srem = makeConstAwareDivOrRem(ctx, BVOp::SRem, a, b);

    BVNodeId remNonZero = ctx.make_binary(BVOp::Ne, srem, zero64);
    BVNodeId aNeg = ctx.make_binary(BVOp::Slt, a, zero64);
    BVNodeId bNeg = ctx.make_binary(BVOp::Slt, b, zero64);
    if (isCeil) {
        BVNodeId sameSign = ctx.make_binary(BVOp::Eq, aNeg, bNeg);
        BVNodeId needInc = ctx.make_binary(BVOp::And, remNonZero, sameSign);
        return ctx.make_ite(needInc, ctx.make_binary(BVOp::Add, sdiv, one64), sdiv);
    } else {
        BVNodeId diffSign = ctx.make_binary(BVOp::Ne, aNeg, bNeg);
        BVNodeId needDec = ctx.make_binary(BVOp::And, remNonZero, diffSign);
        return ctx.make_ite(needDec, ctx.make_binary(BVOp::Sub, sdiv, one64), sdiv);
    }
}

void minimizeCounterexampleModel(const BVContext& ctx,
                                 BVNodeId badCond,
                                 size_t totalVars,
                                 int32_t domainMin,
                                 int32_t domainMax,
                                 std::vector<uint64_t>& bestModel) {
    static constexpr int32_t kSmallCandidates[] = {
        0, 1, -1, 2, -2, 3, -3, 4, -4, 5, 7, 8, -8, 10, 16, 31, 32, -32, 64, 100, -128, 127
    };
    if (totalVars == 1) {
        for (int32_t c0 : kSmallCandidates) {
            if (c0 < domainMin || c0 > domainMax) continue;
            std::vector<uint64_t> cand{static_cast<uint64_t>(static_cast<uint32_t>(c0))};
            if (ctx.eval_concrete(badCond, cand) != 0) {
                bestModel = std::move(cand);
                return;
            }
        }
    } else if (totalVars == 2) {
        for (int32_t c0 : kSmallCandidates) {
            if (c0 < domainMin || c0 > domainMax) continue;
            for (int32_t c1 : kSmallCandidates) {
                if (c1 < domainMin || c1 > domainMax) continue;
                std::vector<uint64_t> cand{static_cast<uint64_t>(static_cast<uint32_t>(c0)),
                                           static_cast<uint64_t>(static_cast<uint32_t>(c1))};
                if (ctx.eval_concrete(badCond, cand) != 0) {
                    bestModel = std::move(cand);
                    return;
                }
            }
        }
    }
}

}  // namespace bronze::check
