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
    return path.rfind("__local_", 0) == 0 || path.rfind("__fn_", 0) == 0;
}

bool isSyntheticNoInitialVarPath(const std::string& path) {
    return isLocalRootPath(path) || path.rfind("__ret.", 0) == 0 || path == "__ret";
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
    BVNodeId var64 = floatMode_ ? ctx_.make_var("heap:" + path, 64, /*is_float=*/true)
                                : ctx_.make_sext(ctx_.make_var("heap:" + path, 32), 32);
    initialVals_[path] = var64;
    varPaths_.push_back(path);
    varNodes_.push_back(var64);
    return var64;
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
        il::ValueId thisParamId = static_cast<il::ValueId>(fn.needsEnv ? 1 : 0);
        objPaths[thisParamId] = thisPath;
        valMap[thisParamId] = table.getObjectToken(thisPath);
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
                                    BVNodeId& outVal,
                                    bool* outIsUndefined) {
    if (outIsUndefined) *outIsUndefined = false;
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
    BVContext& ctx = table.ctx();
    int64_t constIdx = 0;
    std::string base;
    if (parseConstElemPath(path, base, constIdx)) {
        if (constIdx < 0) {
            if (outIsUndefined) *outIsUndefined = true;
            outVal = ctx.make_const(0, 64);
            return true;
        }
        BVNodeId idxConst = ctx.make_const(static_cast<uint64_t>(constIdx), 64);
        bool localBase = isLocalRootPath(base);
        BVNodeId res = localBase ? ctx.make_const(0, 64) : table.getInitialVal(path);
        bool matchedSymWrite = false;
        for (const auto& [wPath, wVal] : writes) {
            std::string symBase;
            BVNodeId symIdx = 0;
            if (parseSymbolicIdxPath(wPath, symBase, symIdx) && symBase == base) {
                BVNodeId eq = ctx.make_binary(BVOp::Eq, idxConst, symIdx);
                res = ctx.make_ite(eq, wVal, res);
                matchedSymWrite = true;
            }
        }
        if (localBase && !matchedSymWrite && outIsUndefined) {
            *outIsUndefined = true;
        }
        outVal = res;
        return true;
    }
    if (isLocalRootPath(it->second)) {
        if (key != "length" && outIsUndefined) *outIsUndefined = true;
        outVal = ctx.make_const(0, 64);
        return true;
    }
    if (key == "length" || key == "_isBigNumber" || key == "isBigNumber" || key == "isDecimal") {
        outVal = ctx.make_const(1, 64);
        return true;
    }
    if (key == "sigBytes") {
        outVal = ctx.make_const(4, 64);
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
                                    BVNodeId& outVal,
                                    bool* outIsUndefined) {
    if (outIsUndefined) *outIsUndefined = false;
    BVContext& ctx = table.ctx();
    if (idxNode < ctx.nodes().size() && ctx.nodes()[idxNode].op == BVOp::Const) {
        int64_t imm = static_cast<int64_t>(ctx.nodes()[idxNode].imm);
        return evalPropGet(table, recvId, std::to_string(imm), resultId, outVal, outIsUndefined);
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
    bool localBase = isLocalRootPath(base);
    BVNodeId res = localBase ? ctx.make_const(0, 64) : table.getInitialVal(symPath);
    bool anyBaseWrite = false;
    for (const auto& [wPath, wVal] : writes) {
        std::string wBase;
        BVNodeId wSymIdx = 0;
        int64_t wConstIdx = 0;
        if (parseSymbolicIdxPath(wPath, wBase, wSymIdx) && wBase == base) {
            BVNodeId eq = ctx.make_binary(BVOp::Eq, idxNode, wSymIdx);
            res = ctx.make_ite(eq, wVal, res);
            anyBaseWrite = true;
        } else if (parseConstElemPath(wPath, wBase, wConstIdx) && wBase == base) {
            BVNodeId cNode = ctx.make_const(static_cast<uint64_t>(wConstIdx), 64);
            BVNodeId eq = ctx.make_binary(BVOp::Eq, idxNode, cNode);
            res = ctx.make_ite(eq, wVal, res);
            anyBaseWrite = true;
        }
    }
    if (localBase && !anyBaseWrite && outIsUndefined) {
        *outIsUndefined = true;
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
    if (isSyntheticNoInitialVarPath(path)) return table.ctx().make_const(0, 64);
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
    for (const auto& [k, v] : fHeap.propFuncRefs) {
        merged.propFuncRefs.try_emplace(k, v);
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

}  // namespace bronze::check
