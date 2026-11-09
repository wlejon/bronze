// Calls of the global builtins (`Math.*`, `Number.*`, `String(x)`,
// `isNaN`, ...) as symbolic expressions: the receivers the symbolic builder
// knows without a function body to inline.
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
        if (globalName == "String" && heapTable.numericParams() &&
            (operands.size() < 3 || !heap.isStringValue(operands[2]))) {
            // Handled, with an error: a false return would let the call fall
            // through to the generic paths, which pass the number through.
            if (outErr) *outErr = kNumberToStringReason;
            return true;
        }
        if (globalName == "Boolean" && operands.size() >= 3 && opVals.size() >= 3 &&
            !heap.isStringValue(operands[2])) {
            // ToBoolean of a number: false for 0 (and NaN), not the number.
            if (floatMode) {
                BVNodeId nonZero = ctx.make_binary(BVOp::FNe, opVals[2], makeF64ConstNode(ctx, 0.0), 1);
                BVNodeId notNaN = ctx.make_binary(BVOp::FEq, opVals[2], opVals[2], 1);
                outVal = makeFloatBool64(ctx, ctx.make_binary(BVOp::And, nonZero, notNaN, 1));
            } else {
                outVal = ctx.make_ite(ctx.make_binary(BVOp::Ne, opVals[2], zero64, 1), one64, zero64);
            }
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
            outVal = evalFloorOrCeilSymbolic(ctx, heap, operands[1], opVals[1], method == "ceil",
                                             heapTable.domainMin(), heapTable.domainMax());
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
        // The builtins whose result is a boolean, which the bit-vector alone
        // would leave a number.
        static const char* const kBoolBuiltins[] = {
            "isNaN", "isFinite", "Boolean", "Number.isNaN", "Number.isFinite", "Number.isInteger",
            "Number.isSafeInteger", "Object.is", "Array.isArray",
        };
        std::string full = method.empty() ? gName : gName + "." + method;
        for (const char* b : kBoolBuiltins) {
            if (full == b) setKind(state, inst.result, kindConst(kKindBool));
        }
    }
    return true;
}

// `global.get` / `name.resolve`: a global's value, or the identity token
// that member reads and calls on it are recognised by.
SymEvalOutcome SymbolicIlBuilder::evalGlobalGetOp(const il::Instruction& inst, SymState& state) {
    std::string gName;
    if (inst.keyIndex < mod_.keyConstants.size()) {
        gName = mod_.keyConstants[inst.keyIndex];
        state.globalRefs[inst.result] = gName;
    }
    if (gName == "undefined") {
        state.values[inst.result] = zero64_;
        state.undefValues.insert(inst.result);
        return {};
    }
    if (gName == "NaN") {
        if (floatMode_) {
            state.values[inst.result] = makeF64ConstNode(ctx_, std::numeric_limits<double>::quiet_NaN());
        } else {
            state.values[inst.result] = zero64_;
            state.undefValues.insert(inst.result);
        }
        return {};
    }
    if (gName == "Infinity") {
        if (!floatMode_) {
            if (inModuleInit_) { state.values[inst.result] = zero64_; return {}; }
            return {false, "Infinity constant requires float mode", false, zero64_, {}, {}, zero1_};
        }
        state.values[inst.result] = makeF64ConstNode(ctx_, std::numeric_limits<double>::infinity());
        return {};
    }
    std::string gPath = "__fn_" + (gName.empty() ? std::string("global") : gName);
    BVNodeId gTok = heapTable_.getObjectToken(gPath);
    for (size_t fi = 0; fi < mod_.functions.size(); ++fi) {
        const auto& cand = mod_.functions[fi];
        if (!cand.isExternal && !cand.isEntryPoint &&
            (cand.name == gName || cand.displayName == gName)) {
            uint32_t fIdx = static_cast<uint32_t>(fi);
            uint32_t capEnv = state.funcCapturedEnvs.count(fIdx) ? state.funcCapturedEnvs[fIdx] : 0;
            state.funcRefs[inst.result] = fIdx;
            state.funcEnvs[inst.result] = capEnv;
            gPath = "__fn_" + std::to_string(fi);
            gTok = ctx_.make_const(0x00F0000000000000ULL |
                                   (static_cast<uint64_t>(capEnv & 0xFFFFu) << 32) | fIdx, 64);
            break;
        }
    }
    state.heap.objPaths[inst.result] = gPath;
    state.values[inst.result] = gTok;
    return {};
}

}  // namespace bronze::check
