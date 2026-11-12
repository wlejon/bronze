#include "check/prove_equiv_sym.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bronze::check {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;

SymbolicIlBuilder::SymbolicIlBuilder(BVContext& ctx,
                                     const il::Module& mod,
                                     bool strictJs32Bitwise,
                                     SymbolicHeapTable* sharedHeapTable)
    : ctx_(ctx),
      mod_(mod),
      strictJs32Bitwise_(strictJs32Bitwise),
      floatMode_(sharedHeapTable ? sharedHeapTable->isFloatMode() : false),
      ownedHeapTable_(ctx, floatMode_),
      heapTable_(sharedHeapTable ? *sharedHeapTable : ownedHeapTable_) {
    if (sharedHeapTable) {
        ownedHeapTable_.setDomainBounds(sharedHeapTable->domainMin(), sharedHeapTable->domainMax());
    }
    zero1_ = ctx_.make_const(0, 1);
    one1_ = ctx_.make_const(1, 1);
    zero64_ = ctx_.make_const(0, 64);
    one64_ = ctx_.make_const(1, 64);
    f64One_ = makeF64ConstNode(ctx_, 1.0);
    allOnes64_ = ctx_.make_const(~uint64_t{0}, 64);
    initModuleState();
}

SymEvalOutcome SymbolicIlBuilder::evalFunction(const il::Function& fn,
                                               const std::vector<BVNodeId>& argNodes,
                                               uint32_t inlineDepth,
                                               const SymbolicHeapState* callerHeap,
                                               const std::string& thisPath,
                                               const std::vector<std::string>* callerArgPaths,
                                               uint32_t callerEnvId,
                                               const std::vector<bool>* callerUndefArgs,
                                               bool isConstructCall,
                                               uint32_t callerThisCtor,
                                               const std::vector<uint32_t>* callerArgCtors,
                                               SymState* callerEnvState) {
    if (fn.blocks.empty() || fn.isExternal) {
        return {false, "external or empty function", false, zero64_, {}, {}, zero1_};
    }
    if (inlineDepth > kMaxInlineDepth) {
        return {false, "call recursion depth exceeded", false, zero64_, {}, {}, zero1_};
    }

    SymState initState = moduleState_;
    if (callerEnvState) {
        for (const auto& [k, v] : callerEnvState->envSlots) initState.envSlots[k] = v;
        for (const auto& [k, v] : callerEnvState->envGlobalRefs) initState.envGlobalRefs[k] = v;
        for (const auto& [k, v] : callerEnvState->envArrays) initState.envArrays[k] = v;
        for (const auto& [k, v] : callerEnvState->envParent) initState.envParent[k] = v;
        for (const auto& [k, v] : callerEnvState->envFuncRefs) initState.envFuncRefs[k] = v;
        for (const auto& [k, v] : callerEnvState->envFuncEnvs) initState.envFuncEnvs[k] = v;
        for (uint64_t k : callerEnvState->envUndef) initState.envUndef.insert(k);
        initState.nextEnvId = std::max(initState.nextEnvId, callerEnvState->nextEnvId);
        for (const auto& [k, v] : callerEnvState->funcCapturedEnvs) initState.funcCapturedEnvs[k] = v;
        for (const auto& [k, v] : callerEnvState->protoToCtor) initState.protoToCtor[k] = v;
        for (const auto& [k, v] : callerEnvState->ctorToProto) initState.ctorToProto[k] = v;
        for (const auto& [k, v] : callerEnvState->funcOwnerCtor) initState.funcOwnerCtor[k] = v;
        if (callerEnvState->moduleCtorIdx != UINT32_MAX) {
            initState.moduleCtorIdx = callerEnvState->moduleCtorIdx;
        }
        for (const auto& [k, v] : callerEnvState->heap.envObjPaths) initState.heap.envObjPaths[k] = v;
        for (const auto& [k, v] : callerEnvState->heap.propFuncEnvs) initState.heap.propFuncEnvs[k] = v;
        for (const auto& s : callerEnvState->heap.stringPaths) initState.heap.stringPaths.insert(s);
        for (const auto& a : callerEnvState->heap.arrayPaths) initState.heap.arrayPaths.insert(a);
    }
    initState.isConstructCall = isConstructCall;
    initState.threwCond = zero1_;
    if (callerHeap) {
        initState.heap.writes = callerHeap->writes;
        initState.heap.propObjAliases = callerHeap->propObjAliases;
        initState.heap.propFuncRefs = callerHeap->propFuncRefs;
        initState.heap.propFuncEnvs = callerHeap->propFuncEnvs;
        initState.heap.propGlobalRefs = callerHeap->propGlobalRefs;
        initState.heap.refinedIntervals = callerHeap->refinedIntervals;
        initState.heap.localObjCounter = callerHeap->localObjCounter;
    }

    uint32_t fnIdx = UINT32_MAX;
    for (size_t fi = 0; fi < mod_.functions.size(); ++fi) {
        if (&mod_.functions[fi] == &fn) {
            fnIdx = static_cast<uint32_t>(fi);
            break;
        }
    }

    uint32_t firstSrc = static_cast<uint32_t>(fn.firstSourceParam());
    for (uint32_t i = 0; i < firstSrc; ++i) initState.values[i] = zero64_;
    if (fn.needsEnv) {
        if (callerEnvId != 0) {
            initState.envIds[0] = callerEnvId;
        } else if (fnIdx != UINT32_MAX && initState.funcCapturedEnvs.count(fnIdx)) {
            initState.envIds[0] = initState.funcCapturedEnvs[fnIdx];
        } else {
            initState.envIds[0] = 0;
        }
    }

    uint32_t ownerCtor = UINT32_MAX;
    if (fnIdx != UINT32_MAX && initState.funcOwnerCtor.count(fnIdx)) {
        ownerCtor = initState.funcOwnerCtor[fnIdx];
    } else if (fn.needsThis && initState.moduleCtorIdx != UINT32_MAX) {
        ownerCtor = initState.moduleCtorIdx;
    }

    if (fn.needsThis) {
        il::ValueId thisParamId = static_cast<il::ValueId>(fn.needsEnv ? 1 : 0);
        if (isConstructCall && fnIdx != UINT32_MAX) {
            initState.ctorOf[thisParamId] = fnIdx;
        } else if (callerThisCtor != UINT32_MAX) {
            initState.ctorOf[thisParamId] = callerThisCtor;
        } else if (inlineDepth == 0 && ownerCtor != UINT32_MAX && fnIdx != ownerCtor) {
            initState.ctorOf[thisParamId] = ownerCtor;
        }
    }

    uint32_t srcCount = static_cast<uint32_t>(fn.callerParamCount());
    for (uint32_t i = 0; i < srcCount; ++i) {
        il::ValueId pid = firstSrc + i;
        initState.values[pid] = (i < argNodes.size()) ? argNodes[i] : zero64_;
        if ((callerUndefArgs && i < callerUndefArgs->size() && (*callerUndefArgs)[i]) || i >= argNodes.size()) {
            initState.undefValues.insert(pid);
        }
        if (callerArgCtors && i < callerArgCtors->size() && (*callerArgCtors)[i] != UINT32_MAX) {
            initState.ctorOf[pid] = (*callerArgCtors)[i];
        } else if (inlineDepth == 0 && fn.needsThis && ownerCtor != UINT32_MAX && fnIdx != ownerCtor) {
            initState.ctorOf[pid] = ownerCtor;
        }
        if (initState.values[pid] < ctx_.nodes().size() &&
            ctx_.nodes()[initState.values[pid]].op == BVOp::Const) {
            uint64_t uImm = ctx_.nodes()[initState.values[pid]].imm;
            if ((uImm >> 48) == 0x00F0u) {
                initState.funcRefs[pid] = static_cast<uint32_t>(uImm & 0xFFFFFFFFu);
                initState.funcEnvs[pid] = static_cast<uint32_t>((uImm >> 32) & 0xFFFFu);
            }
        }
    }
    initState.heap.initFunctionHeap(heapTable_, fn, initState.values, thisPath, callerArgPaths);
    auto out = evalBlockPath(fn, 0, std::move(initState), 0, inlineDepth);
    return out;
}

void SymbolicIlBuilder::initModuleState() {
    for (size_t fi = 0; fi < mod_.functions.size(); ++fi) {
        const auto& f = mod_.functions[fi];
        if (f.isExternal || f.isEntryPoint || f.name == "main") continue;
        const std::string& dn = !f.displayName.empty() ? f.displayName : f.name;
        auto dot = dn.find('.');
        if (dot != std::string::npos && dot > 0) {
            std::string clsName = dn.substr(0, dot);
            for (size_t ci = 0; ci < mod_.functions.size(); ++ci) {
                const auto& cf = mod_.functions[ci];
                if (!cf.isExternal && !cf.isEntryPoint &&
                    (cf.name == clsName || cf.displayName == clsName)) {
                    moduleState_.funcOwnerCtor[static_cast<uint32_t>(fi)] = static_cast<uint32_t>(ci);
                    if (moduleState_.moduleCtorIdx == UINT32_MAX) {
                        moduleState_.moduleCtorIdx = static_cast<uint32_t>(ci);
                    }
                    break;
                }
            }
        }
        for (const auto& blk : f.blocks) {
            std::unordered_map<il::ValueId, uint32_t> localFnRefs;
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::CreateFunction || inst.op == il::Op::FunctionRef) {
                    localFnRefs[inst.result] = inst.calleeIndex;
                } else if ((inst.op == il::Op::PropGet || inst.op == il::Op::PropSet) &&
                           !inst.operands.empty() && inst.keyIndex < mod_.keyConstants.size() &&
                           mod_.keyConstants[inst.keyIndex] == "prototype" &&
                           localFnRefs.count(inst.operands[0])) {
                    moduleState_.moduleCtorIdx = localFnRefs[inst.operands[0]];
                } else if (inst.op == il::Op::Construct && !inst.operands.empty() &&
                           localFnRefs.count(inst.operands[0])) {
                    if (moduleState_.moduleCtorIdx == UINT32_MAX) {
                        moduleState_.moduleCtorIdx = localFnRefs[inst.operands[0]];
                    }
                }
            }
        }
    }

    const il::Function* entryFn = nullptr;
    for (const auto& f : mod_.functions) {
        if (!f.isExternal && (f.isEntryPoint || f.name == "main")) {
            entryFn = &f;
            break;
        }
    }
    if (!entryFn || entryFn->blocks.empty()) return;
    inModuleInit_ = true;
    uint32_t savedVisits = totalVisits_;
    bool savedHit = hitLoopUnrollBound_;
    uint32_t savedLoopBound = maxLoopBoundUsed_;
    SymState initSt = moduleState_;
    initSt.threwCond = zero1_;
    initSt.exceededCond = zero1_;
    uint32_t firstSrc = static_cast<uint32_t>(entryFn->firstSourceParam());
    for (uint32_t i = 0; i < firstSrc; ++i) initSt.values[i] = zero64_;
    initSt.heap.initFunctionHeap(heapTable_, *entryFn, initSt.values, "__module_this", nullptr);
    auto out = evalBlockPath(*entryFn, 0, std::move(initSt), 0, 0);
    if (out.supported) {
        moduleState_.heap.writes = std::move(out.finalHeap.writes);
        moduleState_.heap.propFuncRefs = std::move(out.finalHeap.propFuncRefs);
        moduleState_.heap.propFuncEnvs = std::move(out.finalHeap.propFuncEnvs);
        moduleState_.heap.propGlobalRefs = std::move(out.finalHeap.propGlobalRefs);
        moduleState_.heap.localObjCounter = out.finalHeap.localObjCounter;
    }
    inModuleInit_ = false;
    totalVisits_ = savedVisits;
    hitLoopUnrollBound_ = savedHit;
    maxLoopBoundUsed_ = savedLoopBound;
}

uint64_t SymbolicIlBuilder::resolveEnvSlotKey(const SymState& st,
                                              il::ValueId envVal,
                                              uint32_t depth,
                                              uint32_t index) const {
    uint32_t cur = 0;
    auto it = st.envIds.find(envVal);
    if (it != st.envIds.end()) cur = it->second;
    for (uint32_t d = 0; d < depth; ++d) {
        auto pIt = st.envParent.find(cur);
        if (pIt != st.envParent.end()) cur = pIt->second;
        else cur = 0;
    }
    return (static_cast<uint64_t>(cur) << 32) | index;
}

BVNodeId SymbolicIlBuilder::getVal(const SymState& st, il::ValueId v) const {
    auto it = st.values.find(v);
    return (it != st.values.end()) ? it->second : zero64_;
}

BVNodeId SymbolicIlBuilder::toBool1(BVNodeId node) {
    if (ctx_.width(node) == 1) return node;
    BVNodeId b64 = toBV64(node);
    const brass::verify::BVNode n = ctx_.nodes()[b64];
    if (!floatMode_) {
        if (n.op == BVOp::ZExt && ctx_.width(n.a) == 1) return n.a;
        return ctx_.make_binary(BVOp::Ne, b64, zero64_);
    }
    if (n.op == BVOp::Ite && n.b == f64One_ && n.c == zero64_) return n.a;
    return ctx_.make_binary(BVOp::And, ctx_.make_binary(BVOp::FEq, b64, b64, 1),
                            ctx_.make_binary(BVOp::FNe, b64, zero64_, 1), 1);
}

BVNodeId SymbolicIlBuilder::toBV64(BVNodeId node) {
    uint32_t w = ctx_.width(node);
    if (w >= 64) return node;
    if (w == 1) return floatMode_ ? ctx_.make_ite(node, f64One_, zero64_) : ctx_.make_zext(node, 63);
    return floatMode_ ? ctx_.make_unary(BVOp::SiToFp, node, 64) : ctx_.make_sext(node, 64 - w);
}

BVNodeId SymbolicIlBuilder::toSInt32In64(BVNodeId node) {
    if (floatMode_) {
        return ctx_.make_unary(BVOp::SiToFp, evalFloatToInt32Node(ctx_, toBV64(node)), 64);
    }
    return ctx_.make_sext(ctx_.make_extract(toBV64(node), 31, 0), 32);
}

BVNodeId SymbolicIlBuilder::kindConst(uint64_t kind) const {
    return ctx_.make_const(kind, kKindWidth);
}

BVNodeId SymbolicIlBuilder::kindOf(const SymState& st, il::ValueId v) const {
    auto it = st.valueKind.find(v);
    return it == st.valueKind.end() ? kindConst(kKindNumber) : it->second;
}

void SymbolicIlBuilder::setKind(SymState& st, il::ValueId v, BVNodeId kind) const {
    if (kind == kindConst(kKindNumber)) st.valueKind.erase(v);
    else st.valueKind[v] = kind;
}

BVNodeId SymbolicIlBuilder::returnKindOf(const SymEvalOutcome& out) const {
    return out.returnKind == kNoKind ? kindConst(kKindNumber) : out.returnKind;
}

BVNodeId SymbolicIlBuilder::mergeKinds(BVNodeId cond, BVNodeId tKind, BVNodeId fKind) {
    return tKind == fKind ? tKind : ctx_.make_ite(cond, tKind, fKind);
}

void SymbolicIlBuilder::applyEdgeArgsInPlace(const il::Function& fn,
                                             const il::BlockTarget& edge,
                                             SymState& state) {
    if (edge.block >= fn.blocks.size()) return;
    const auto& targetBlk = fn.blocks[edge.block];
    std::vector<std::pair<il::ValueId, BVNodeId>> updates;
    std::vector<std::pair<il::ValueId, bool>> undefUpdates;
    std::vector<std::pair<il::ValueId, BVNodeId>> kindUpdates;
    updates.reserve(targetBlk.params.size());
    for (size_t k = 0; k < targetBlk.params.size() && k < edge.args.size(); ++k) {
        il::ValueId src = edge.args[k], dst = targetBlk.params[k].id;
        updates.emplace_back(dst, toBV64(getVal(state, src)));
        undefUpdates.emplace_back(dst, state.undefValues.count(src) > 0);
        kindUpdates.emplace_back(dst, kindOf(state, src));
        state.heap.propagateAlias(src, dst);
        if (state.funcRefs.count(src)) state.funcRefs[dst] = state.funcRefs[src];
        if (state.funcEnvs.count(src)) state.funcEnvs[dst] = state.funcEnvs[src];
        if (state.ctorOf.count(src)) state.ctorOf[dst] = state.ctorOf[src];
        if (state.globalRefs.count(src)) state.globalRefs[dst] = state.globalRefs[src];
        if (state.envIds.count(src)) state.envIds[dst] = state.envIds[src];
        if (state.arrayValues.count(src)) state.arrayValues[dst] = state.arrayValues[src];
    }
    for (const auto& [dst, val] : updates) state.values[dst] = val;
    for (const auto& [dst, isU] : undefUpdates) {
        if (isU) state.undefValues.insert(dst);
        else state.undefValues.erase(dst);
    }
    for (const auto& [dst, kind] : kindUpdates) setKind(state, dst, kind);
}

void SymbolicIlBuilder::saveModuleInitSnapshot(const SymState& state) {
    if (!inModuleInit_) return;
    for (const auto& [k, v] : state.envSlots) moduleState_.envSlots[k] = v;
    for (const auto& [k, v] : state.envGlobalRefs) moduleState_.envGlobalRefs[k] = v;
    for (const auto& [k, v] : state.envArrays) moduleState_.envArrays[k] = v;
    for (const auto& [k, v] : state.envParent) moduleState_.envParent[k] = v;
    for (const auto& [k, v] : state.envFuncRefs) moduleState_.envFuncRefs[k] = v;
    for (const auto& [k, v] : state.envFuncEnvs) moduleState_.envFuncEnvs[k] = v;
    for (uint64_t k : state.envUndef) moduleState_.envUndef.insert(k);
    moduleState_.nextEnvId = std::max(moduleState_.nextEnvId, state.nextEnvId);
    for (const auto& [k, v] : state.funcCapturedEnvs) moduleState_.funcCapturedEnvs[k] = v;
    for (const auto& [k, v] : state.protoToCtor) moduleState_.protoToCtor[k] = v;
    for (const auto& [k, v] : state.ctorToProto) moduleState_.ctorToProto[k] = v;
    for (const auto& [k, v] : state.funcOwnerCtor) moduleState_.funcOwnerCtor[k] = v;
    if (state.moduleCtorIdx != UINT32_MAX) moduleState_.moduleCtorIdx = state.moduleCtorIdx;
    for (const auto& [k, v] : state.heap.envObjPaths) moduleState_.heap.envObjPaths[k] = v;
    for (const auto& [k, v] : state.heap.writes) moduleState_.heap.writes[k] = v;
    for (const auto& [k, v] : state.heap.propFuncRefs) moduleState_.heap.propFuncRefs[k] = v;
    for (const auto& [k, v] : state.heap.propFuncEnvs) moduleState_.heap.propFuncEnvs[k] = v;
    for (const auto& [k, v] : state.heap.propGlobalRefs) moduleState_.heap.propGlobalRefs[k] = v;
    for (const auto& s : state.heap.stringPaths) moduleState_.heap.stringPaths.insert(s);
    for (const auto& a : state.heap.arrayPaths) moduleState_.heap.arrayPaths.insert(a);
    moduleState_.heap.localObjCounter = std::max(moduleState_.heap.localObjCounter, state.heap.localObjCounter);
}

const il::Function* findMethodCalleeInModule(const il::Module& mod,
                                             const SymbolicHeapState& heap,
                                             const std::string& recvPath,
                                             const std::string& method) {
    if (!recvPath.empty()) {
        auto pIt = heap.propFuncRefs.find(recvPath + "." + method);
        if (pIt != heap.propFuncRefs.end() && pIt->second < mod.functions.size()) {
            return &mod.functions[pIt->second];
        }
    }
    std::string dotMethod = "." + method;
    for (const auto& [k, fnIdx] : heap.propFuncRefs) {
        if (fnIdx < mod.functions.size() && k.size() > dotMethod.size() &&
            k.compare(k.size() - dotMethod.size(), dotMethod.size(), dotMethod) == 0) {
            return &mod.functions[fnIdx];
        }
    }
    std::string underMethod = "_" + method;
    for (const auto& cand : mod.functions) {
        if (cand.isExternal || cand.isEntryPoint || cand.name == "main") continue;
        if (cand.name == method || cand.displayName == method ||
            (cand.displayName.size() > dotMethod.size() &&
             cand.displayName.compare(cand.displayName.size() - dotMethod.size(),
                                      dotMethod.size(), dotMethod) == 0) ||
            (cand.name.size() > underMethod.size() &&
             cand.name.compare(cand.name.size() - underMethod.size(),
                               underMethod.size(), underMethod) == 0)) {
            return &cand;
        }
    }
    return nullptr;
}

SymEvalOutcome SymbolicIlBuilder::invokeCallee(const il::Function& callee,
                                               const il::Instruction& inst,
                                               size_t startOp,
                                               const std::string& thisPath,
                                               uint32_t envId,
                                               uint32_t inlineDepth,
                                               SymState& state,
                                               bool writeScalarRet,
                                               bool isConstructCall) {
    uint32_t calleeIdx = UINT32_MAX;
    for (size_t fi = 0; fi < mod_.functions.size(); ++fi) {
        if (&mod_.functions[fi] == &callee) {
            calleeIdx = static_cast<uint32_t>(fi);
            break;
        }
    }
    if (envId == 0 && calleeIdx != UINT32_MAX && state.funcCapturedEnvs.count(calleeIdx)) {
        envId = state.funcCapturedEnvs[calleeIdx];
    }
    uint32_t callerThisCtor = UINT32_MAX;
    std::string effThis = thisPath.empty() ? "this" : thisPath;
    for (const auto& [vid, p] : state.heap.objPaths) {
        if (p == effThis && state.ctorOf.count(vid)) {
            callerThisCtor = state.ctorOf[vid];
            break;
        }
    }
    std::vector<BVNodeId> callArgs;
    std::vector<std::string> callPaths;
    std::vector<bool> callUndef;
    std::vector<uint32_t> callCtors;
    for (size_t k = startOp; k < inst.operands.size(); ++k) {
        il::ValueId opId = inst.operands[k];
        callArgs.push_back(toBV64(getVal(state, opId)));
        callPaths.push_back(state.heap.lookupObjPath(opId));
        callUndef.push_back(state.undefValues.count(opId) > 0);
        callCtors.push_back(state.ctorOf.count(opId) ? state.ctorOf[opId] : UINT32_MAX);
    }
    auto sub = evalFunction(callee, callArgs, inlineDepth + 1, &state.heap,
                            effThis, &callPaths, envId, &callUndef, isConstructCall,
                            callerThisCtor, &callCtors, &state);
    if (!sub.supported || !sub.hasReturn) {
        return {false, sub.unsupportedReason.empty() ? "callee hit loop unroll bound" : sub.unsupportedReason,
                false, zero64_, {}, {}, zero1_};
    }
    BVNodeId calleeThrew = (sub.threwNode != 0) ? sub.threwNode : zero1_;
    if (calleeThrew < ctx_.nodes().size() && ctx_.nodes()[calleeThrew].op == BVOp::Const &&
        ctx_.nodes()[calleeThrew].imm != 0) {
        return {true, {}, true, zero64_, std::move(state.heap), {}, one1_};
    }
    if (calleeThrew != zero1_) {
        state.threwCond = (state.threwCond == 0 || state.threwCond == zero1_)
                              ? calleeThrew
                              : ctx_.make_binary(BVOp::Or, state.threwCond, calleeThrew);
        SymbolicHeapState merged = SymbolicHeapState::mergeBranches(
            heapTable_, calleeThrew, state.heap, sub.finalHeap);
        state.heap.writes = std::move(merged.writes);
    } else {
        state.heap.writes = std::move(sub.finalHeap.writes);
    }
    if (sub.exceededBoundNode != 0 && sub.exceededBoundNode != zero1_) {
        state.exceededCond = (state.exceededCond == 0 || state.exceededCond == zero1_)
                                 ? sub.exceededBoundNode
                                 : ctx_.make_binary(BVOp::Or, state.exceededCond, sub.exceededBoundNode);
    }
    state.heap.propObjAliases = std::move(sub.finalHeap.propObjAliases);
    state.heap.propFuncRefs = std::move(sub.finalHeap.propFuncRefs);
    state.heap.propFuncEnvs = std::move(sub.finalHeap.propFuncEnvs);
    state.heap.propGlobalRefs = std::move(sub.finalHeap.propGlobalRefs);
    for (const auto& [nid, iv] : sub.finalHeap.refinedIntervals) {
        if (nid == sub.returnVal) state.heap.refinedIntervals[nid] = iv;
    }
    for (const auto& s : sub.finalHeap.stringPaths) state.heap.stringPaths.insert(s);
    for (const auto& a : sub.finalHeap.arrayPaths) state.heap.arrayPaths.insert(a);
    state.heap.localObjCounter = sub.finalHeap.localObjCounter;
    if (inModuleInit_) {
        for (const auto& [k, v] : moduleState_.envSlots) state.envSlots[k] = v;
        for (const auto& [k, v] : moduleState_.envGlobalRefs) state.envGlobalRefs[k] = v;
        for (const auto& [k, v] : moduleState_.envArrays) state.envArrays[k] = v;
        for (const auto& [k, v] : moduleState_.envParent) state.envParent[k] = v;
        for (const auto& [k, v] : moduleState_.envFuncRefs) state.envFuncRefs[k] = v;
        for (const auto& [k, v] : moduleState_.envFuncEnvs) state.envFuncEnvs[k] = v;
        state.nextEnvId = std::max(state.nextEnvId, moduleState_.nextEnvId);
        for (const auto& [k, v] : moduleState_.funcCapturedEnvs) state.funcCapturedEnvs[k] = v;
        for (const auto& [k, v] : moduleState_.protoToCtor) state.protoToCtor[k] = v;
        for (const auto& [k, v] : moduleState_.ctorToProto) state.ctorToProto[k] = v;
        for (const auto& [k, v] : moduleState_.funcOwnerCtor) state.funcOwnerCtor[k] = v;
        if (moduleState_.moduleCtorIdx != UINT32_MAX) state.moduleCtorIdx = moduleState_.moduleCtorIdx;
    }
    if (writeScalarRet && inst.result != il::kNoValue) state.values[inst.result] = sub.returnVal;
    if (!sub.returnObjPath.empty() && inst.result != il::kNoValue) {
        state.heap.objPaths[inst.result] = sub.returnObjPath;
        state.values[inst.result] = sub.returnVal;
    }
    if (inst.result != il::kNoValue) {
        setKind(state, inst.result, returnKindOf(sub));
        if (sub.returnFuncRef != UINT32_MAX) {
            state.funcRefs[inst.result] = sub.returnFuncRef;
            state.funcEnvs[inst.result] = sub.returnFuncEnv;
        }
        if (sub.returnCtorOf != UINT32_MAX) {
            state.ctorOf[inst.result] = sub.returnCtorOf;
        }
        if (!sub.returnGlobalRef.empty()) {
            state.globalRefs[inst.result] = sub.returnGlobalRef;
        }
        if (!sub.returnArray.empty()) {
            state.arrayValues[inst.result] = sub.returnArray;
        }
    }
    return {true, {}, false, zero64_, {}, {}, zero1_};
}

SymEvalOutcome SymbolicIlBuilder::evalConstructOp(const il::Instruction& inst,
                                                  uint32_t inlineDepth,
                                                  SymState& state) {
    uint32_t ctorIdx = UINT32_MAX;
    uint32_t ctorEnv = 0;
    if (!inst.operands.empty()) {
        il::ValueId op0 = inst.operands[0];
        if (state.funcRefs.count(op0)) {
            ctorIdx = state.funcRefs[op0];
            ctorEnv = state.funcEnvs.count(op0) ? state.funcEnvs[op0] : 0;
        } else {
            BVNodeId v0 = toBV64(getVal(state, op0));
            if (v0 < ctx_.nodes().size() && ctx_.nodes()[v0].op == BVOp::Const &&
                (ctx_.nodes()[v0].imm >> 48) == 0x00F0u) {
                ctorIdx = static_cast<uint32_t>(ctx_.nodes()[v0].imm & 0xFFFFFFFFu);
                ctorEnv = static_cast<uint32_t>((ctx_.nodes()[v0].imm >> 32) & 0xFFFFu);
            }
        }
    }
    if (!inModuleInit_ && !inst.operands.empty() &&
        ctorIdx == UINT32_MAX && !state.globalRefs.count(inst.operands[0])) {
        return {false, "unsupported IL operation: construct on unknown target", false, zero64_, {}, {}, zero1_};
    }
    state.values[inst.result] = state.heap.createLocalObject(heapTable_, inst.result);
    std::string localPath = state.heap.lookupObjPath(inst.result);
    if (ctorIdx != UINT32_MAX) {
        state.ctorOf[inst.result] = ctorIdx;
        if (ctorIdx < mod_.functions.size() && !mod_.functions[ctorIdx].isExternal) {
            return invokeCallee(mod_.functions[ctorIdx], inst, 1, localPath,
                                ctorEnv, inlineDepth, state, false, true);
        }
    }
    if (inst.operands.size() >= 2) {
        il::ValueId arg1 = inst.operands[1];
        auto arrIt = state.arrayValues.find(arg1);
        std::string arg1Path = state.heap.lookupObjPath(arg1);
        if (arrIt != state.arrayValues.end()) {
            state.arrayValues[inst.result] = arrIt->second;
            state.heap.evalPropSet(heapTable_, inst.result, "length", ctx_.make_const(arrIt->second.size(), 64));
            for (size_t idx = 0; idx < arrIt->second.size(); ++idx) {
                state.heap.evalPropSet(heapTable_, inst.result, std::to_string(idx), arrIt->second[idx]);
            }
        } else if (!arg1Path.empty() && state.heap.writes.count(arg1Path + ".length")) {
            state.heap.objPaths[inst.result] = arg1Path;
            state.values[inst.result] = toBV64(getVal(state, arg1));
        } else {
            state.heap.evalPropSet(heapTable_, inst.result, "length", toBV64(getVal(state, arg1)), arg1);
            if (inst.operands.size() >= 3) {
                state.heap.evalPropSet(heapTable_, inst.result, "sign", toBV64(getVal(state, inst.operands[2])), inst.operands[2]);
            }
        }
    } else {
        state.heap.evalPropSet(heapTable_, inst.result, "length", zero64_);
    }
    return {true, {}, false, zero64_, {}, {}, zero1_};
}

SymEvalOutcome SymbolicIlBuilder::evalMethodCallOp(const il::Instruction& inst,
                                                   uint32_t inlineDepth,
                                                   SymState& state) {
    if (inst.operands.empty() || inst.keyIndex >= mod_.keyConstants.size()) {
        return {false, "unsupported dynamic method call", false, zero64_, {}, {}, zero1_};
    }
    const std::string& method = mod_.keyConstants[inst.keyIndex];
    if (state.globalRefs.count(inst.operands[0])) {
        std::string gErr;
        if (tryEvalGlobalCall(state.globalRefs[inst.operands[0]], method, inst, state, gErr)) {
            if (!gErr.empty()) return {false, gErr, false, zero64_, {}, {}, zero1_};
            return {true, {}, false, zero64_, {}, {}, zero1_};
        }
    }
    SymEvalOutcome seqOut;
    if (tryEvalSeqMethodCallOp(method, inst, state, seqOut)) {
        return seqOut;
    }
    std::string recvPath = state.heap.lookupObjPath(inst.operands[0]);
    const il::Function* callee = (inst.directTarget != il::Instruction::kNoDirectTarget &&
                                  inst.directTarget < mod_.functions.size())
                                     ? &mod_.functions[inst.directTarget]
                                     : findMethodCalleeInModule(mod_, state.heap, recvPath, method);
    if (callee && !callee->isExternal) {
        return invokeCallee(*callee, inst, 1, recvPath, 0, inlineDepth, state);
    }
    if (inModuleInit_) {
        state.values[inst.result] = zero64_;
        return {true, {}, false, zero64_, {}, {}, zero1_};
    }
    return {false, "unsupported method call: " + method, false, zero64_, {}, {}, zero1_};
}

namespace {

struct FuncIteArm {
    BVNodeId cond;
    uint32_t calleeIdx;
    uint32_t capEnv;
};

bool collectFuncIteArms(BVContext& ctx,
                        BVNodeId node,
                        BVNodeId pathCond,
                        std::vector<FuncIteArm>& arms,
                        BVNodeId& outInvalidCond,
                        int depth = 0) {
    if (depth > 8 || node >= ctx.nodes().size()) return false;
    const brass::verify::BVNode n = ctx.nodes()[node];
    if (n.op == BVOp::Const) {
        if ((n.imm >> 48) == 0x00F0u) {
            arms.push_back({pathCond, static_cast<uint32_t>(n.imm & 0xFFFFFFFFu),
                            static_cast<uint32_t>((n.imm >> 32) & 0xFFFFu)});
            return true;
        }
        if (n.imm == 0) {
            outInvalidCond = (outInvalidCond == 0) ? pathCond : ctx.make_binary(BVOp::Or, outInvalidCond, pathCond, 1);
            return true;
        }
        return false;
    }
    if (n.op == BVOp::Ite) {
        BVNodeId tCond = ctx.make_binary(BVOp::And, pathCond, n.a, 1);
        BVNodeId fCond = ctx.make_binary(BVOp::And, pathCond, ctx.make_unary(BVOp::Not, n.a, 1), 1);
        return collectFuncIteArms(ctx, n.b, tCond, arms, outInvalidCond, depth + 1) &&
               collectFuncIteArms(ctx, n.c, fCond, arms, outInvalidCond, depth + 1);
    }
    return false;
}

}  // namespace

SymEvalOutcome SymbolicIlBuilder::evalDynamicCallOp(const il::Instruction& inst,
                                                    uint32_t inlineDepth,
                                                    SymState& state) {
    if (!inst.operands.empty() && state.globalRefs.count(inst.operands[0])) {
        std::string gErr;
        if (tryEvalGlobalCall(state.globalRefs[inst.operands[0]], "", inst, state, gErr)) {
            if (!gErr.empty()) return {false, gErr, false, zero64_, {}, {}, zero1_};
            return {true, {}, false, zero64_, {}, {}, zero1_};
        }
    }
    if (!inst.operands.empty()) {
        il::ValueId op0 = inst.operands[0];
        uint32_t calleeIdx = UINT32_MAX;
        uint32_t capEnv = 0;
        if (state.funcRefs.count(op0)) {
            calleeIdx = state.funcRefs[op0];
            capEnv = state.funcEnvs.count(op0) ? state.funcEnvs[op0] : 0;
        } else {
            BVNodeId v0 = toBV64(getVal(state, op0));
            if (v0 < ctx_.nodes().size() && ctx_.nodes()[v0].op == BVOp::Const &&
                (ctx_.nodes()[v0].imm >> 48) == 0x00F0u) {
                calleeIdx = static_cast<uint32_t>(ctx_.nodes()[v0].imm & 0xFFFFFFFFu);
                capEnv = static_cast<uint32_t>((ctx_.nodes()[v0].imm >> 32) & 0xFFFFu);
            } else if (v0 < ctx_.nodes().size() && ctx_.nodes()[v0].op == BVOp::Ite) {
                std::vector<FuncIteArm> arms;
                BVNodeId invalidCond = 0;
                if (collectFuncIteArms(ctx_, v0, one1_, arms, invalidCond) && !arms.empty() && arms.size() <= 8) {
                    std::string thisPath = (inst.operands.size() > 1) ? state.heap.lookupObjPath(inst.operands[1]) : std::string{};
                    BVNodeId mergedVal = zero64_;
                    BVNodeId mergedKind = kindConst(kKindNumber);
                    SymbolicHeapState mergedHeap = state.heap;
                    for (const auto& arm : arms) {
                        if (arm.calleeIdx >= mod_.functions.size() || mod_.functions[arm.calleeIdx].isExternal) {
                            return {false, "external dynamic callee", false, zero64_, {}, {}, zero1_};
                        }
                        SymState armState = state;
                        auto armOut = invokeCallee(mod_.functions[arm.calleeIdx], inst, 2, thisPath, arm.capEnv, inlineDepth, armState);
                        if (!armOut.supported) return armOut;
                        BVNodeId armRet = (inst.result != il::kNoValue) ? toBV64(getVal(armState, inst.result)) : zero64_;
                        mergedVal = ctx_.make_ite(arm.cond, armRet, mergedVal);
                        if (inst.result != il::kNoValue) {
                            mergedKind = mergeKinds(arm.cond, kindOf(armState, inst.result), mergedKind);
                        }
                        mergedHeap = SymbolicHeapState::mergeBranches(heapTable_, arm.cond, armState.heap, mergedHeap);
                        if (armState.threwCond != 0 && armState.threwCond != zero1_) {
                            BVNodeId armThrow = ctx_.make_binary(BVOp::And, arm.cond, armState.threwCond, 1);
                            state.threwCond = (state.threwCond == 0 || state.threwCond == zero1_)
                                                  ? armThrow : ctx_.make_binary(BVOp::Or, state.threwCond, armThrow, 1);
                        }
                    }
                    if (invalidCond != 0) {
                        state.threwCond = (state.threwCond == 0 || state.threwCond == zero1_)
                                              ? invalidCond : ctx_.make_binary(BVOp::Or, state.threwCond, invalidCond, 1);
                    }
                    state.heap.writes = std::move(mergedHeap.writes);
                    if (inst.result != il::kNoValue) {
                        state.values[inst.result] = mergedVal;
                        setKind(state, inst.result, mergedKind);
                    }
                    return {true, {}, false, zero64_, {}, {}, zero1_};
                }
            }
        }
        if (calleeIdx != UINT32_MAX) {
            if (calleeIdx >= mod_.functions.size() || mod_.functions[calleeIdx].isExternal) {
                return {false, "external dynamic callee", false, zero64_, {}, {}, zero1_};
            }
            std::string thisPath = (inst.operands.size() > 1) ? state.heap.lookupObjPath(inst.operands[1]) : std::string{};
            return invokeCallee(mod_.functions[calleeIdx], inst, 2, thisPath, capEnv, inlineDepth, state);
        }
    }
    if (inModuleInit_) {
        state.values[inst.result] = zero64_;
        return {true, {}, false, zero64_, {}, {}, zero1_};
    }
    return {false, "unsupported IL operation: call.dynamic", false, zero64_, {}, {}, zero1_};
}

SymEvalOutcome SymbolicIlBuilder::evalDirectCallOp(const il::Instruction& inst,
                                                   uint32_t inlineDepth,
                                                   SymState& state) {
    if (inst.calleeIndex >= mod_.functions.size()) {
        return {false, "invalid callee index", false, zero64_, {}, {}, zero1_};
    }
    const il::Function& callee = mod_.functions[inst.calleeIndex];
    uint32_t calleeEnvId = 0;
    if (callee.needsEnv && !inst.operands.empty()) {
        uint32_t cur = state.envIds.count(inst.operands[0]) ? state.envIds[inst.operands[0]] : 0;
        uint32_t hops = (inst.callEnvHops != il::Instruction::kNoEnvHops) ? inst.callEnvHops : 0;
        for (uint32_t h = 0; h < hops; ++h) {
            cur = state.envParent.count(cur) ? state.envParent[cur] : 0;
        }
        calleeEnvId = cur;
    }
    std::string thisPath = "this";
    if (callee.needsThis && (callee.needsEnv ? 1u : 0u) < inst.operands.size()) {
        std::string p = state.heap.lookupObjPath(inst.operands[callee.needsEnv ? 1 : 0]);
        if (!p.empty()) thisPath = std::move(p);
    }
    size_t startOp = (inst.operands.size() == callee.params.size())
                         ? callee.firstSourceParam()
                         : ((inst.callEnvHops != il::Instruction::kNoEnvHops && !inst.operands.empty()) ? 1 : 0);
    return invokeCallee(callee, inst, startOp, thisPath, calleeEnvId, inlineDepth, state);
}

}  // namespace bronze::check
