#include "check/prove_equiv_sym.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bronze::check {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;

namespace {

BVNodeId makeFloatBool64(BVContext& ctx, BVNodeId cond1) {
    return ctx.make_ite(cond1, makeF64ConstNode(ctx, 1.0), ctx.make_const(0, 64));
}

}  // namespace

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
                                               bool isConstructCall) {
    if (fn.blocks.empty() || fn.isExternal) {
        return {false, "external or empty function", false, zero64_, {}, {}, zero1_};
    }
    if (inlineDepth > kMaxInlineDepth) {
        return {false, "call recursion depth exceeded", false, zero64_, {}, {}, zero1_};
    }

    SymState initState = moduleState_;
    initState.isConstructCall = isConstructCall;
    initState.threwCond = zero1_;
    if (callerHeap) {
        initState.heap.writes = callerHeap->writes;
        initState.heap.propFuncRefs = callerHeap->propFuncRefs;
        initState.heap.propGlobalRefs = callerHeap->propGlobalRefs;
        initState.heap.localObjCounter = callerHeap->localObjCounter;
    }
    uint32_t firstSrc = static_cast<uint32_t>(fn.firstSourceParam());
    for (uint32_t i = 0; i < firstSrc; ++i) initState.values[i] = zero64_;
    if (fn.needsEnv) initState.envIds[0] = callerEnvId;
    uint32_t srcCount = static_cast<uint32_t>(fn.callerParamCount());
    for (uint32_t i = 0; i < srcCount; ++i) {
        initState.values[firstSrc + i] = (i < argNodes.size()) ? argNodes[i] : zero64_;
        if ((callerUndefArgs && i < callerUndefArgs->size() && (*callerUndefArgs)[i]) || i >= argNodes.size()) {
            initState.undefValues.insert(firstSrc + i);
        }
    }
    initState.heap.initFunctionHeap(heapTable_, fn, initState.values, thisPath, callerArgPaths);
    return evalBlockPath(fn, 0, initState, 0, inlineDepth);
}

void SymbolicIlBuilder::initModuleState() {
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
    SymState initSt;
    initSt.threwCond = zero1_;
    initSt.exceededCond = zero1_;
    uint32_t firstSrc = static_cast<uint32_t>(entryFn->firstSourceParam());
    for (uint32_t i = 0; i < firstSrc; ++i) initSt.values[i] = zero64_;
    initSt.heap.initFunctionHeap(heapTable_, *entryFn, initSt.values, "this", nullptr);
    auto out = evalBlockPath(*entryFn, 0, std::move(initSt), 0, 0);
    if (out.supported) {
        moduleState_.heap.writes = std::move(out.finalHeap.writes);
        moduleState_.heap.propFuncRefs = std::move(out.finalHeap.propFuncRefs);
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
    if (!floatMode_) return ctx_.make_binary(BVOp::Ne, b64, zero64_);
    const auto& n = ctx_.nodes()[b64];
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

void SymbolicIlBuilder::applyEdgeArgsInPlace(const il::Function& fn,
                                             const il::BlockTarget& edge,
                                             SymState& state) {
    if (edge.block >= fn.blocks.size()) return;
    const auto& targetBlk = fn.blocks[edge.block];
    std::vector<std::pair<il::ValueId, BVNodeId>> updates;
    std::vector<std::pair<il::ValueId, bool>> undefUpdates;
    updates.reserve(targetBlk.params.size());
    for (size_t k = 0; k < targetBlk.params.size() && k < edge.args.size(); ++k) {
        il::ValueId src = edge.args[k], dst = targetBlk.params[k].id;
        updates.emplace_back(dst, toBV64(getVal(state, src)));
        undefUpdates.emplace_back(dst, state.undefValues.count(src) > 0);
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
}

void SymbolicIlBuilder::saveModuleInitSnapshot(const SymState& state) {
    if (!inModuleInit_) return;
    moduleState_.envSlots = state.envSlots;
    moduleState_.envGlobalRefs = state.envGlobalRefs;
    moduleState_.envArrays = state.envArrays;
    moduleState_.envParent = state.envParent;
    moduleState_.envFuncRefs = state.envFuncRefs;
    moduleState_.envFuncEnvs = state.envFuncEnvs;
    moduleState_.envUndef = state.envUndef;
    moduleState_.nextEnvId = state.nextEnvId;
    moduleState_.heap.envObjPaths = state.heap.envObjPaths;
    moduleState_.heap.writes = state.heap.writes;
    moduleState_.heap.propFuncRefs = state.heap.propFuncRefs;
    moduleState_.heap.propGlobalRefs = state.heap.propGlobalRefs;
    moduleState_.heap.localObjCounter = state.heap.localObjCounter;
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

bool evalGlobalMethodCallSymbolic(BVContext& ctx,
                                  SymbolicHeapTable& heapTable,
                                  SymbolicHeapState& heap,
                                  const std::string& globalName,
                                  const std::string& method,
                                  const std::vector<il::ValueId>& operands,
                                  const std::vector<BVNodeId>& opVals,
                                  il::ValueId resultId,
                                  BVNodeId& outVal,
                                  std::vector<BVNodeId>* outArrayLiteral,
                                  std::string* outErr) {
    BVNodeId zero64 = ctx.make_const(0, 64);
    BVNodeId one64 = ctx.make_const(1, 64);
    BVNodeId allOnes64 = ctx.make_const(~uint64_t{0}, 64);
    const bool floatMode = heapTable.isFloatMode();

    if (method.empty()) {
        size_t dot = globalName.find('.');
        if (dot != std::string::npos) {
            std::vector<il::ValueId> sOps{operands.empty() ? 0 : operands[0]};
            std::vector<BVNodeId> sVals{zero64};
            for (size_t k = 2; k < operands.size() && k < opVals.size(); ++k) {
                sOps.push_back(operands[k]);
                sVals.push_back(opVals[k]);
            }
            return evalGlobalMethodCallSymbolic(ctx, heapTable, heap, globalName.substr(0, dot),
                                                globalName.substr(dot + 1), sOps, sVals,
                                                resultId, outVal, outArrayLiteral, outErr);
        }
        if (globalName == "Error" || globalName == "RangeError" ||
            globalName == "TypeError" || globalName == "SyntaxError") {
            outVal = heap.createLocalObject(heapTable, resultId);
            return true;
        }
        if (globalName == "String" || globalName == "Number" || globalName == "Boolean" ||
            globalName == "Object") {
            outVal = (opVals.size() >= 3) ? opVals[2] : zero64;
            if (operands.size() >= 3) heap.propagateAlias(operands[2], resultId);
            return true;
        }
        if (globalName == "isFinite") {
            if (floatMode && opVals.size() >= 3) {
                BVNodeId posInf = makeF64ConstNode(ctx, std::numeric_limits<double>::infinity());
                outVal = makeFloatBool64(ctx, ctx.make_binary(BVOp::FLt, ctx.make_unary(BVOp::FAbs, opVals[2], 64), posInf, 1));
                return true;
            }
            outVal = one64;
            return true;
        }
        if (globalName == "isNaN") {
            if (floatMode && opVals.size() >= 3) {
                outVal = makeFloatBool64(ctx, ctx.make_binary(BVOp::FNe, opVals[2], opVals[2], 1));
                return true;
            }
            outVal = zero64;
            return true;
        }
        if (globalName == "Array" || globalName == "Uint8Array" || globalName == "Int32Array" ||
            globalName == "Uint32Array" || globalName == "Float64Array") {
            outVal = heap.createLocalObject(heapTable, resultId);
            heap.evalPropSet(heapTable, resultId, "length", (opVals.size() >= 3) ? opVals[2] : zero64);
            return true;
        }
        return false;
    }

    if (globalName == "Math") {
        if (method == "abs" && opVals.size() >= 2) {
            BVNodeId a = opVals[1];
            if (floatMode) {
                outVal = ctx.make_unary(BVOp::FAbs, a, 64);
                return true;
            }
            outVal = ctx.make_ite(ctx.make_binary(BVOp::Slt, a, zero64),
                                  ctx.make_unary(BVOp::Neg, a), a);
            return true;
        }
        if ((method == "min" || method == "max") && opVals.size() >= 3) {
            BVNodeId acc = opVals[1];
            for (size_t k = 2; k < opVals.size(); ++k) {
                if (floatMode) {
                    acc = ctx.make_binary((method == "min") ? BVOp::FMin : BVOp::FMax, acc, opVals[k], 64);
                } else {
                    BVNodeId pick = (method == "min")
                                        ? ctx.make_binary(BVOp::Slt, opVals[k], acc)
                                        : ctx.make_binary(BVOp::Slt, acc, opVals[k]);
                    acc = ctx.make_ite(pick, opVals[k], acc);
                }
            }
            outVal = acc;
            return true;
        }
        if (method == "imul" && opVals.size() >= 3) {
            BVNodeId a32 = floatMode ? evalFloatToInt32Node(ctx, opVals[1]) : ctx.make_extract(opVals[1], 31, 0);
            BVNodeId b32 = floatMode ? evalFloatToInt32Node(ctx, opVals[2]) : ctx.make_extract(opVals[2], 31, 0);
            BVNodeId prod32 = ctx.make_binary(BVOp::Mul, a32, b32);
            outVal = floatMode ? ctx.make_unary(BVOp::SiToFp, prod32, 64) : ctx.make_sext(prod32, 32);
            return true;
        }
        if (method == "sign" && opVals.size() >= 2) {
            BVNodeId a = opVals[1];
            if (floatMode) {
                outVal = ctx.make_ite(
                    ctx.make_binary(BVOp::FNe, a, a, 1), a,
                    ctx.make_ite(ctx.make_binary(BVOp::FEq, a, zero64, 1), a,
                                 ctx.make_ite(ctx.make_binary(BVOp::FLt, a, zero64, 1),
                                              makeF64ConstNode(ctx, -1.0), makeF64ConstNode(ctx, 1.0))));
                return true;
            }
            BVNodeId isNeg = ctx.make_binary(BVOp::Slt, a, zero64);
            BVNodeId isPos = ctx.make_binary(BVOp::Slt, zero64, a);
            outVal = ctx.make_ite(isNeg, allOnes64, ctx.make_ite(isPos, one64, zero64));
            return true;
        }
        if (method == "clz32" && opVals.size() >= 2) {
            BVNodeId in64 = floatMode ? ctx.make_sext(evalFloatToInt32Node(ctx, opVals[1]), 32) : opVals[1];
            BVNodeId clz = makeClz32Symbolic(ctx, in64);
            outVal = floatMode ? ctx.make_unary(BVOp::SiToFp, ctx.make_extract(clz, 31, 0), 64) : clz;
            return true;
        }
        if (method == "log2" && opVals.size() >= 2 && !floatMode) {
            outVal = ctx.make_binary(BVOp::Sub, ctx.make_const(31, 64),
                                     makeClz32Symbolic(ctx, opVals[1]));
            return true;
        }
        if (method == "pow" && opVals.size() >= 3) {
            bool ok = false;
            outVal = floatMode ? evalFloatPowSymbolic(ctx, opVals[1], opVals[2], ok)
                               : evalPowSymbolic(ctx, opVals[1], opVals[2], ok,
                                                 heapTable.domainMin(), heapTable.domainMax());
            if (!ok && outErr) *outErr = "unsupported symbolic Math.pow";
            return true;
        }
        if ((method == "floor" || method == "ceil") && opVals.size() >= 2) {
            if (floatMode) {
                outVal = (method == "ceil") ? evalFloatCeilSymbolic(ctx, opVals[1])
                                            : evalFloatFloorSymbolic(ctx, opVals[1]);
                return true;
            }
            outVal = evalFloorOrCeilSymbolic(ctx, heap, operands[1], opVals[1], method == "ceil");
            return true;
        }
        if (method == "trunc" && opVals.size() >= 2) {
            if (floatMode) {
                outVal = evalFloatTruncSymbolic(ctx, opVals[1]);
                return true;
            }
            outVal = opVals[1];
            return true;
        }
        if (method == "round" && opVals.size() >= 2) {
            if (floatMode) {
                outVal = evalFloatRoundSymbolic(ctx, opVals[1]);
                return true;
            }
            if (operands.size() >= 2 && heap.divResults.count(operands[1])) {
                if (outErr) *outErr = "unsupported fractional Math.round";
                return true;
            }
            outVal = opVals[1];
            return true;
        }
        if (method == "fround" && opVals.size() >= 2) {
            if (floatMode) {
                outVal = ctx.make_unary(BVOp::FpExt, ctx.make_unary(BVOp::FpTrunc, opVals[1], 32), 64);
                return true;
            }
            if (operands.size() >= 2 && heap.divResults.count(operands[1])) {
                if (outErr) *outErr = "unsupported fractional Math.fround";
                return true;
            }
            outVal = opVals[1];
            return true;
        }
        if (method == "sqrt" && opVals.size() >= 2 && floatMode) {
            outVal = ctx.make_unary(BVOp::FSqrt, opVals[1], 64);
            return true;
        }
    }
    if (globalName == "Number" && opVals.size() >= 2) {
        if (floatMode) {
            BVNodeId a = opVals[1];
            if (method == "isNaN") {
                outVal = makeFloatBool64(ctx, ctx.make_binary(BVOp::FNe, a, a, 1));
                return true;
            }
            if (method == "isFinite") {
                BVNodeId posInf = makeF64ConstNode(ctx, std::numeric_limits<double>::infinity());
                outVal = makeFloatBool64(ctx, ctx.make_binary(BVOp::FLt, ctx.make_unary(BVOp::FAbs, a, 64), posInf, 1));
                return true;
            }
            if (method == "isInteger") {
                BVNodeId posInf = makeF64ConstNode(ctx, std::numeric_limits<double>::infinity());
                BVNodeId fin = ctx.make_binary(BVOp::FLt, ctx.make_unary(BVOp::FAbs, a, 64), posInf, 1);
                BVNodeId intEq = ctx.make_binary(BVOp::FEq, evalFloatFloorSymbolic(ctx, a), a, 1);
                outVal = makeFloatBool64(ctx, ctx.make_binary(BVOp::And, fin, intEq, 1));
                return true;
            }
        }
        if (method == "isFinite" || method == "isSafeInteger" || method == "isInteger") {
            outVal = one64;
            return true;
        }
        if (method == "isNaN") {
            outVal = zero64;
            return true;
        }
    }
    if (globalName == "String" &&
        (method == "fromCharCode" || method == "fromCodePoint") && opVals.size() >= 2) {
        BoundedSeq seq;
        seq.isString = true;
        size_t count = std::min<size_t>(opVals.size() - 1, kMaxBoundedSeqLen);
        seq.len = ctx.make_const(count, 64);
        std::vector<BVNodeId> chars;
        for (size_t i = 0; i < kMaxBoundedSeqLen; ++i) {
            if (i < count) {
                BVNodeId ch = ctx.make_binary(BVOp::And, opVals[i + 1], ctx.make_const(0xFFFF, 64));
                seq.elems[i] = ch;
                chars.push_back(ch);
            } else {
                seq.elems[i] = zero64;
            }
        }
        heap.createLocalObject(heapTable, resultId);
        std::string p = heap.lookupObjPath(resultId);
        if (!p.empty()) {
            heap.stringPaths.insert(p);
            heap.writes[p + ".length"] = seq.len;
            for (size_t i = 0; i < kMaxBoundedSeqLen; ++i) {
                heap.writes[p + "." + std::to_string(i)] = seq.elems[i];
            }
        }
        outVal = packBoundedStringNode(ctx, seq.len, seq.elems);
        if (outArrayLiteral) *outArrayLiteral = std::move(chars);
        return true;
    }
    if (globalName == "Object") {
        if (method == "is" && opVals.size() >= 3) {
            if (floatMode) {
                BVNodeId a = opVals[1], b = opVals[2];
                BVNodeId bothNan = ctx.make_binary(
                    BVOp::And, ctx.make_binary(BVOp::FNe, a, a, 1), ctx.make_binary(BVOp::FNe, b, b, 1), 1);
                BVNodeId sameBits = ctx.make_binary(BVOp::Eq, a, b, 1);
                outVal = makeFloatBool64(ctx, ctx.make_binary(BVOp::Or, bothNan, sameBits, 1));
            } else {
                outVal = ctx.make_zext(ctx.make_binary(BVOp::Eq, opVals[1], opVals[2], 1), 63);
            }
            return true;
        }
        if (method == "setPrototypeOf" && operands.size() >= 2) {
            outVal = opVals[1];
            heap.propagateAlias(operands[1], resultId);
            return true;
        }
        if (method == "create") {
            outVal = heap.createLocalObject(heapTable, resultId);
            return true;
        }
    }
    if (globalName == "Array" && method == "isArray") {
        outVal = floatMode ? makeF64ConstNode(ctx, 1.0) : one64;
        return true;
    }
    return false;
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
    std::vector<BVNodeId> callArgs;
    std::vector<std::string> callPaths;
    std::vector<bool> callUndef;
    for (size_t k = startOp; k < inst.operands.size(); ++k) {
        callArgs.push_back(toBV64(getVal(state, inst.operands[k])));
        callPaths.push_back(state.heap.lookupObjPath(inst.operands[k]));
        callUndef.push_back(state.undefValues.count(inst.operands[k]) > 0);
    }
    auto sub = evalFunction(callee, callArgs, inlineDepth + 1, &state.heap,
                            thisPath.empty() ? "this" : thisPath, &callPaths, envId, &callUndef, isConstructCall);
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
    state.heap.propFuncRefs = std::move(sub.finalHeap.propFuncRefs);
    state.heap.localObjCounter = sub.finalHeap.localObjCounter;
    if (writeScalarRet) state.values[inst.result] = sub.returnVal;
    if (!sub.returnObjPath.empty() && inst.result != il::kNoValue) {
        state.heap.objPaths[inst.result] = sub.returnObjPath;
        state.values[inst.result] = sub.returnVal;
    }
    return {true, {}, false, zero64_, {}, {}, zero1_};
}

bool SymbolicIlBuilder::tryEvalGlobalCall(const std::string& gName,
                                          const std::string& method,
                                          const il::Instruction& inst,
                                          SymState& state,
                                          std::string& outErr) {
    std::vector<BVNodeId> opVals;
    for (il::ValueId opId : inst.operands) opVals.push_back(toBV64(getVal(state, opId)));
    BVNodeId gOut = zero64_;
    std::vector<BVNodeId> gArr;
    if (!evalGlobalMethodCallSymbolic(ctx_, heapTable_, state.heap, gName, method,
                                      inst.operands, opVals, inst.result, gOut, &gArr, &outErr)) {
        return false;
    }
    if (outErr.empty()) {
        state.values[inst.result] = gOut;
        if (!gArr.empty()) state.arrayValues[inst.result] = std::move(gArr);
    }
    return true;
}

SymEvalOutcome SymbolicIlBuilder::evalConstructOp(const il::Instruction& inst,
                                                  uint32_t inlineDepth,
                                                  SymState& state) {
    if (!inModuleInit_ && !inst.operands.empty() &&
        !state.funcRefs.count(inst.operands[0]) && !state.globalRefs.count(inst.operands[0])) {
        return {false, "unsupported IL operation: construct on unknown target", false, zero64_, {}, {}, zero1_};
    }
    state.values[inst.result] = state.heap.createLocalObject(heapTable_, inst.result);
    std::string localPath = state.heap.lookupObjPath(inst.result);
    if (!inst.operands.empty() && state.funcRefs.count(inst.operands[0])) {
        uint32_t ctorIdx = state.funcRefs[inst.operands[0]];
        state.ctorOf[inst.result] = ctorIdx;
        uint32_t ctorEnv = state.funcEnvs.count(inst.operands[0]) ? state.funcEnvs[inst.operands[0]] : 0;
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
    if (inst.operands.empty() || !state.funcRefs.count(inst.operands[0])) {
        if (inModuleInit_) {
            state.values[inst.result] = zero64_;
            return {true, {}, false, zero64_, {}, {}, zero1_};
        }
        return {false, "unsupported IL operation: call.dynamic", false, zero64_, {}, {}, zero1_};
    }
    uint32_t calleeIdx = state.funcRefs[inst.operands[0]];
    if (calleeIdx >= mod_.functions.size() || mod_.functions[calleeIdx].isExternal) {
        return {false, "external dynamic callee", false, zero64_, {}, {}, zero1_};
    }
    uint32_t capEnv = state.funcEnvs.count(inst.operands[0]) ? state.funcEnvs[inst.operands[0]] : 0;
    std::string thisPath = (inst.operands.size() > 1) ? state.heap.lookupObjPath(inst.operands[1]) : std::string{};
    return invokeCallee(mod_.functions[calleeIdx], inst, 2, thisPath, capEnv, inlineDepth, state);
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
