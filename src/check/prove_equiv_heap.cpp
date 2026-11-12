#include "check/prove_equiv_heap.h"

#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_set>

namespace bronze::check {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;

namespace {

bool isLocalRootPath(const std::string& path) {
    return path.rfind("__local_", 0) == 0 || path.rfind("__fn_", 0) == 0 || path.rfind("__module_this", 0) == 0;
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

BVNodeId SymbolicHeapTable::getBoundedLenVal(const std::string& path, bool isString) {
    if (isString) usedBoundedStringLen_ = true;
    else usedBoundedArrayLen_ = true;
    boundedLenPaths_.insert(path);
    auto dot = path.rfind(".length");
    if (dot != std::string::npos) {
        if (isString) stringRoots_.insert(path.substr(0, dot));
        else arrayRoots_.insert(path.substr(0, dot));
    }
    return getInitialVal(path);
}

BVNodeId SymbolicHeapTable::getCharCodeVal(const std::string& path) {
    usedBoundedStringLen_ = true;
    charCodePaths_.insert(path);
    return getInitialVal(path);
}

void SymbolicHeapTable::markStringRoot(const std::string& basePath) {
    if (basePath.empty()) return;
    stringRoots_.insert(basePath);
    usedBoundedStringLen_ = true;
}

void SymbolicHeapTable::markArrayRoot(const std::string& basePath) {
    if (basePath.empty()) return;
    arrayRoots_.insert(basePath);
    usedBoundedArrayLen_ = true;
}

BVNodeId SymbolicHeapTable::buildBoundedSeqConstraintCond() {
    if (boundedLenPaths_.empty() && charCodePaths_.empty()) return 0;
    BVNodeId conj = 0;
    auto andCond = [&](BVNodeId c) {
        if (c == 0) return;
        conj = (conj == 0) ? c : ctx_.make_binary(BVOp::And, conj, c, 1);
    };
    BVNodeId zero64 = ctx_.make_const(0, 64);
    BVNodeId maxLen64 = ctx_.make_const(kMaxBoundedSeqLen, 64);
    BVNodeId maxChar64 = ctx_.make_const(127, 64);
    BVNodeId one64 = ctx_.make_const(1, 64);
    for (const std::string& p : boundedLenPaths_) {
        BVNodeId v = getInitialVal(p);
        BVNodeId minLen = (p.rfind("this.", 0) == 0 && p != "this.length") ? one64 : zero64;
        andCond(ctx_.make_binary(BVOp::Sge, v, minLen, 1));
        andCond(ctx_.make_binary(BVOp::Sle, v, maxLen64, 1));
    }
    for (const std::string& p : charCodePaths_) {
        BVNodeId v = getInitialVal(p);
        andCond(ctx_.make_binary(BVOp::Sge, v, zero64, 1));
        andCond(ctx_.make_binary(BVOp::Sle, v, maxChar64, 1));
    }
    return conj;
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

std::string SymbolicHeapState::resolveObjPath(const std::string& p) const {
    std::string cur = p;
    for (int hop = 0; hop < 8; ++hop) {
        auto it = propObjAliases.find(cur);
        if (it == propObjAliases.end() || it->second == cur) break;
        cur = it->second;
    }
    return cur;
}

bool SymbolicHeapState::isStringPath(const std::string& p) const {
    if (p.empty()) return false;
    return stringPaths.count(p) > 0 || stringPaths.count(resolveObjPath(p)) > 0;
}

bool SymbolicHeapState::isStringValue(il::ValueId id) const {
    auto it = objPaths.find(id);
    return (it != objPaths.end()) && isStringPath(it->second);
}

std::string SymbolicHeapState::lookupObjPath(il::ValueId id) const {
    auto it = objPaths.find(id);
    return (it != objPaths.end()) ? resolveObjPath(it->second) : std::string{};
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
    BVContext& ctx = table.ctx();
    int32_t dMin = table.domainMin();
    int32_t dMax = table.domainMax();
    auto tighten = [&](auto& selfT, BVNodeId id, int64_t lo, int64_t hi, int depth) -> void {
        if (id >= ctx.nodes().size() || lo > hi || depth <= 0) return;
        auto cur = getSignedInterval64(ctx, id, dMin, dMax, 16, &refinedIntervals);
        cur.smin = std::max(cur.smin, lo);
        cur.smax = std::min(cur.smax, hi);
        if (cur.smin <= cur.smax) refinedIntervals[id] = cur;
        const brass::verify::BVNode n = ctx.nodes()[id];
        if (n.op == BVOp::Add && ctx.nodes()[n.b].op == BVOp::Const) {
            int64_t c = static_cast<int64_t>(ctx.nodes()[n.b].imm);
            int64_t nLo = (lo == INT64_MIN) ? INT64_MIN : (lo - c);
            int64_t nHi = (hi == INT64_MAX) ? INT64_MAX : (hi - c);
            selfT(selfT, n.a, nLo, nHi, depth - 1);
        } else if (n.op == BVOp::Add && ctx.nodes()[n.a].op == BVOp::Const) {
            int64_t c = static_cast<int64_t>(ctx.nodes()[n.a].imm);
            int64_t nLo = (lo == INT64_MIN) ? INT64_MIN : (lo - c);
            int64_t nHi = (hi == INT64_MAX) ? INT64_MAX : (hi - c);
            selfT(selfT, n.b, nLo, nHi, depth - 1);
        } else if (n.op == BVOp::Sub && ctx.nodes()[n.b].op == BVOp::Const) {
            int64_t c = static_cast<int64_t>(ctx.nodes()[n.b].imm);
            int64_t nLo = (lo == INT64_MIN) ? INT64_MIN : (lo + c);
            int64_t nHi = (hi == INT64_MAX) ? INT64_MAX : (hi + c);
            selfT(selfT, n.a, nLo, nHi, depth - 1);
        } else if (n.op == BVOp::Sub && ctx.nodes()[n.a].op == BVOp::Const) {
            int64_t c = static_cast<int64_t>(ctx.nodes()[n.a].imm);
            int64_t nLo = (hi == INT64_MAX) ? INT64_MIN : (c - hi);
            int64_t nHi = (lo == INT64_MIN) ? INT64_MAX : (c - lo);
            selfT(selfT, n.b, nLo, nHi, depth - 1);
        }
    };
    auto applyTighten = [&](BVNodeId id, int64_t lo, int64_t hi) {
        tighten(tighten, id, lo, hi, 4);
    };
    auto refineCond = [&](auto& self, BVNodeId cId, bool truth) -> void {
        if (cId >= ctx.nodes().size()) return;
        const brass::verify::BVNode cn = ctx.nodes()[cId];
        if (cn.op == BVOp::ZExt || cn.op == BVOp::SExt) {
            self(self, cn.a, truth);
            return;
        }
        if (cn.op == BVOp::Ne && ctx.nodes()[cn.b].op == BVOp::Const && ctx.nodes()[cn.b].imm == 0) {
            self(self, cn.a, truth);
            return;
        }
        if (cn.op == BVOp::Eq && ctx.nodes()[cn.b].op == BVOp::Const && ctx.nodes()[cn.b].imm == 0) {
            self(self, cn.a, !truth);
            return;
        }
        if (cn.op == BVOp::Not) {
            self(self, cn.a, !truth);
            return;
        }
        if (cn.op == BVOp::And && truth) {
            self(self, cn.a, true);
            self(self, cn.b, true);
            return;
        }
        if (cn.op == BVOp::Or && !truth) {
            self(self, cn.a, false);
            self(self, cn.b, false);
            return;
        }
        if (cn.op != BVOp::Slt && cn.op != BVOp::Sle) return;
        BVNodeId L = cn.a;
        BVNodeId R = cn.b;
        bool strict = (cn.op == BVOp::Slt);
        auto lIv = getSignedInterval64(ctx, L, dMin, dMax, 16, &refinedIntervals);
        auto rIv = getSignedInterval64(ctx, R, dMin, dMax, 16, &refinedIntervals);
        if (truth) {
            int64_t d = strict ? 1 : 0;
            if (rIv.smax < INT64_MAX) applyTighten(L, INT64_MIN, rIv.smax - d);
            if (lIv.smin > INT64_MIN) applyTighten(R, lIv.smin + d, INT64_MAX);
            applyTighten(ctx.make_binary(BVOp::Sub, L, R), INT64_MIN, -d);
            applyTighten(ctx.make_binary(BVOp::Sub, R, L), d, INT64_MAX);
            if (ctx.nodes()[R].op == BVOp::Add && ctx.nodes()[ctx.nodes()[R].b].op == BVOp::Const) {
                int64_t c = static_cast<int64_t>(ctx.nodes()[ctx.nodes()[R].b].imm);
                applyTighten(ctx.make_binary(BVOp::Sub, L, ctx.nodes()[R].a), INT64_MIN, c - d);
            }
            if (ctx.nodes()[L].op == BVOp::Add && ctx.nodes()[ctx.nodes()[L].b].op == BVOp::Const) {
                int64_t c = static_cast<int64_t>(ctx.nodes()[ctx.nodes()[L].b].imm);
                applyTighten(ctx.make_binary(BVOp::Sub, R, ctx.nodes()[L].a), d - c, INT64_MAX);
            }
        } else {
            int64_t d = strict ? 0 : 1;
            if (rIv.smin > INT64_MIN) applyTighten(L, rIv.smin + d, INT64_MAX);
            if (lIv.smax < INT64_MAX) applyTighten(R, INT64_MIN, lIv.smax - d);
            applyTighten(ctx.make_binary(BVOp::Sub, L, R), d, INT64_MAX);
            applyTighten(ctx.make_binary(BVOp::Sub, R, L), INT64_MIN, -d);
            if (ctx.nodes()[R].op == BVOp::Add && ctx.nodes()[ctx.nodes()[R].b].op == BVOp::Const) {
                int64_t c = static_cast<int64_t>(ctx.nodes()[ctx.nodes()[R].b].imm);
                applyTighten(ctx.make_binary(BVOp::Sub, L, ctx.nodes()[R].a), c + d, INT64_MAX);
            }
            if (ctx.nodes()[L].op == BVOp::Add && ctx.nodes()[ctx.nodes()[L].b].op == BVOp::Const) {
                int64_t c = static_cast<int64_t>(ctx.nodes()[ctx.nodes()[L].b].imm);
                applyTighten(ctx.make_binary(BVOp::Sub, R, ctx.nodes()[L].a), INT64_MIN, c - d);
            }
        }
    };
    refineCond(refineCond, condBool1, isTrueBranch);

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
    std::string recvBase = resolveObjPath(it->second);
    std::string path = recvBase + "." + key;
    if (resultId != il::kNoValue) {
        objPaths[resultId] = resolveObjPath(path);
    }
    auto wIt = writes.find(path);
    if (wIt != writes.end()) {
        outVal = wIt->second;
        return true;
    }
    if (propObjAliases.count(path)) {
        outVal = table.getObjectToken(resolveObjPath(path));
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
        bool isStrBase = isStringPath(base) || table.isStringRoot(base);
        BVNodeId res = localBase ? ctx.make_const(0, 64)
                                 : (isStrBase ? table.getCharCodeVal(path) : table.getInitialVal(path));
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
    if (isLocalRootPath(recvBase)) {
        if (key != "length" && outIsUndefined) *outIsUndefined = true;
        outVal = ctx.make_const(0, 64);
        return true;
    }
    if (key == "length") {
        bool asStr = isStringPath(recvBase) || table.isStringRoot(recvBase) || table.preferStringSeq();
        outVal = table.getBoundedLenVal(path, asStr);
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
    BVContext& ctx = table.ctx();
    auto it = objPaths.find(recvId);
    if (it == objPaths.end() || key.empty()) return false;
    std::string recvBase = resolveObjPath(it->second);
    std::string path = recvBase + "." + key;
    writes[path] = valNode;
    if (key == "length") {
        BVNodeId zero64 = ctx.make_const(0, 64);
        for (size_t idx = 0; idx < kMaxBoundedSeqLen; ++idx) {
            std::string elemKey = recvBase + "." + std::to_string(idx);
            auto eIt = writes.find(elemKey);
            if (eIt != writes.end()) {
                BVNodeId inB = ctx.make_binary(BVOp::Ult, ctx.make_const(idx, 64), valNode, 1);
                eIt->second = ctx.make_ite(inB, eIt->second, zero64);
            } else if (!isLocalRootPath(recvBase) && table.hasBoundedLen(recvBase)) {
                BVNodeId inB = ctx.make_binary(BVOp::Ult, ctx.make_const(idx, 64), valNode, 1);
                writes[elemKey] = ctx.make_ite(inB, table.getInitialVal(elemKey), zero64);
            }
        }
    }
    if (valId != il::kNoValue && key != "constructor") {
        auto vObj = objPaths.find(valId);
        if (vObj != objPaths.end()) {
            std::string srcBase = resolveObjPath(vObj->second);
            if (srcBase.rfind("__fn_", 0) != 0) {
                propObjAliases[path] = srcBase;
                if (stringPaths.count(srcBase)) stringPaths.insert(path);
                if (arrayPaths.count(srcBase)) arrayPaths.insert(path);
                std::vector<std::pair<std::string, BVNodeId>> copied;
                std::string pref = srcBase + ".";
                for (const auto& [k, v] : writes) {
                    if (k.rfind(pref, 0) == 0) {
                        copied.emplace_back(path + k.substr(srcBase.size()), v);
                    }
                }
                for (const auto& [ck, cv] : copied) writes[ck] = cv;
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
        uint64_t uImm = ctx.nodes()[idxNode].imm;
        if ((uImm >> 60) == 0x5ULL) {
            size_t sLen = static_cast<size_t>((uImm >> 56) & 0xFULL);
            std::string sKey;
            for (size_t i = 0; i < sLen && i < kMaxBoundedSeqLen; ++i) {
                sKey.push_back(static_cast<char>((uImm >> (i * 7)) & 0x7FULL));
            }
            return evalPropGet(table, recvId, sKey, resultId, outVal, outIsUndefined);
        }
        int64_t imm = static_cast<int64_t>(uImm);
        return evalPropGet(table, recvId, std::to_string(imm), resultId, outVal, outIsUndefined);
    }
    auto it = objPaths.find(recvId);
    if (it == objPaths.end()) return false;
    std::string base = resolveObjPath(it->second);
    if (table.hasBoundedLen(base) || stringPaths.count(base) || arrayPaths.count(base)) {
        BVNodeId lenNode = 0;
        evalPropGet(table, recvId, "length", il::kNoValue, lenNode);
        BVNodeId res = ctx.make_const(0, 64);
        for (size_t k = 0; k < kMaxBoundedSeqLen; ++k) {
            BVNodeId elemK = 0;
            evalPropGet(table, recvId, std::to_string(k), il::kNoValue, elemK);
            BVNodeId kConst = ctx.make_const(k, 64);
            BVNodeId hit = ctx.make_binary(
                BVOp::And, ctx.make_binary(BVOp::Eq, idxNode, kConst, 1),
                ctx.make_binary(BVOp::Ult, kConst, lenNode, 1), 1);
            res = ctx.make_ite(hit, elemK, res);
        }
        outVal = res;
        return true;
    }
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
        } else if (localBase && wPath.rfind(base + ".", 0) == 0) {
            std::string propKey = wPath.substr(base.size() + 1);
            if (propKey != "length" && propKey.find('.') == std::string::npos &&
                propKey.size() <= kMaxBoundedSeqLen) {
                BVNodeId cNode = ctx.make_const(packConcreteString64(propKey), 64);
                BVNodeId eq = ctx.make_binary(BVOp::Eq, idxNode, cNode);
                res = ctx.make_ite(eq, wVal, res);
                anyBaseWrite = true;
            }
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
    if (path.size() > 7 && path.compare(path.size() - 7, 7, ".length") == 0) {
        std::string base = path.substr(0, path.size() - 7);
        if (table.hasBoundedLen(base)) {
            return table.getBoundedLenVal(path, table.isStringRoot(base));
        }
    }
    int64_t constIdx = 0;
    std::string base;
    if (parseConstElemPath(path, base, constIdx) && constIdx >= 0 &&
        static_cast<size_t>(constIdx) < kMaxBoundedSeqLen && table.hasBoundedLen(base)) {
        BVContext& ctx = table.ctx();
        BVNodeId lenVal = getFinalValue(table, base + ".length");
        BVNodeId inB = ctx.make_binary(BVOp::Ult, ctx.make_const(static_cast<uint64_t>(constIdx), 64), lenVal, 1);
        BVNodeId raw = table.isStringRoot(base) ? table.getCharCodeVal(path) : table.getInitialVal(path);
        return ctx.make_ite(inB, raw, ctx.make_const(0, 64));
    }
    return table.getInitialVal(path);
}

int tryFoldRefinedBranchCond(
    const BVContext& ctx,
    BVNodeId condNode,
    int32_t dMin,
    int32_t dMax,
    const std::unordered_map<BVNodeId, brass::verify::BVInterval>& refined) {
    if (condNode >= ctx.nodes().size()) return -1;
    const auto& n = ctx.nodes()[condNode];
    if (n.op == BVOp::Const) return (n.imm != 0) ? 1 : 0;
    if (n.op == BVOp::Not) {
        int sub = tryFoldRefinedBranchCond(ctx, n.a, dMin, dMax, refined);
        return (sub < 0) ? -1 : (1 - sub);
    }
    if (n.op == BVOp::Slt || n.op == BVOp::Sle) {
        auto lIv = getSignedInterval64(ctx, n.a, dMin, dMax, 16, &refined);
        auto rIv = getSignedInterval64(ctx, n.b, dMin, dMax, 16, &refined);
        if (n.op == BVOp::Slt) {
            if (lIv.smax < rIv.smin) return 1;
            if (lIv.smin >= rIv.smax) return 0;
        } else {
            if (lIv.smax <= rIv.smin) return 1;
            if (lIv.smin > rIv.smax) return 0;
        }
    }
    return -1;
}

SymbolicHeapState SymbolicHeapState::mergeBranches(SymbolicHeapTable& table,
                                                   BVNodeId condNode,
                                                   const SymbolicHeapState& tHeap,
                                                   const SymbolicHeapState& fHeap,
                                                   BVNodeId tRet,
                                                   BVNodeId tThrew,
                                                   BVNodeId fRet,
                                                   BVNodeId fThrew,
                                                   BVNodeId mergedRet) {
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
    for (const auto& [k, v] : fHeap.propObjAliases) {
        merged.propObjAliases.try_emplace(k, v);
    }
    for (const auto& s : fHeap.stringPaths) merged.stringPaths.insert(s);
    for (const auto& a : fHeap.arrayPaths) merged.arrayPaths.insert(a);
    merged.localObjCounter = std::max(tHeap.localObjCounter, fHeap.localObjCounter);

    bool tAlwaysThrows = (tThrew != 0 && tThrew < ctx.nodes().size() &&
                          ctx.nodes()[tThrew].op == BVOp::Const && ctx.nodes()[tThrew].imm != 0);
    bool fAlwaysThrows = (fThrew != 0 && fThrew < ctx.nodes().size() &&
                          ctx.nodes()[fThrew].op == BVOp::Const && ctx.nodes()[fThrew].imm != 0);
    if (tAlwaysThrows && !fAlwaysThrows) {
        merged.refinedIntervals = fHeap.refinedIntervals;
    } else if (fAlwaysThrows && !tAlwaysThrows) {
        merged.refinedIntervals = tHeap.refinedIntervals;
    } else {
        merged.refinedIntervals.clear();
        for (const auto& [nid, tIv] : tHeap.refinedIntervals) {
            auto fIt = fHeap.refinedIntervals.find(nid);
            if (fIt != fHeap.refinedIntervals.end()) {
                brass::verify::BVInterval mIv = tIv;
                mIv.smin = std::min(tIv.smin, fIt->second.smin);
                mIv.smax = std::max(tIv.smax, fIt->second.smax);
                merged.refinedIntervals[nid] = mIv;
            }
        }
    }
    if (mergedRet != 0) {
        int32_t dMin = table.domainMin();
        int32_t dMax = table.domainMax();
        auto tIv = getSignedInterval64(ctx, tRet, dMin, dMax, 16, &tHeap.refinedIntervals);
        auto fIv = getSignedInterval64(ctx, fRet, dMin, dMax, 16, &fHeap.refinedIntervals);
        if (tAlwaysThrows && !fAlwaysThrows) {
            merged.refinedIntervals[mergedRet] = fIv;
        } else if (fAlwaysThrows && !tAlwaysThrows) {
            merged.refinedIntervals[mergedRet] = tIv;
        } else {
            brass::verify::BVInterval mIv = tIv;
            mIv.smin = std::min(tIv.smin, fIv.smin);
            mIv.smax = std::max(tIv.smax, fIv.smax);
            merged.refinedIntervals[mergedRet] = mIv;
        }
    }

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

void minimizeFloatCounterexampleModel(const BVContext& ctx,
                                      BVNodeId badCond,
                                      size_t totalVars,
                                      std::vector<uint64_t>& bestModel) {
    const double kFloatCandidates[] = {
        0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, 3.0, -3.0, 4.0, -4.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()
    };
    auto toBits = [](double d) {
        uint64_t b = 0;
        std::memcpy(&b, &d, sizeof(double));
        return b;
    };
    if (totalVars == 1) {
        for (double d0 : kFloatCandidates) {
            std::vector<uint64_t> cand{toBits(d0)};
            if (ctx.eval_concrete(badCond, cand) != 0) {
                bestModel = std::move(cand);
                return;
            }
        }
    } else if (totalVars == 2) {
        for (double d0 : kFloatCandidates) {
            for (double d1 : kFloatCandidates) {
                std::vector<uint64_t> cand{toBits(d0), toBits(d1)};
                if (ctx.eval_concrete(badCond, cand) != 0) {
                    bestModel = std::move(cand);
                    return;
                }
            }
        }
    }
}

}  // namespace bronze::check
