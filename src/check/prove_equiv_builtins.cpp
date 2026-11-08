#include "check/prove_equiv_heap.h"

#include <cstdlib>
#include <unordered_set>

namespace bronze::check {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;

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

uint64_t hashStringLiteral(std::string_view s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return 0x5354520000000000ULL | static_cast<uint64_t>(h);
}

BVNodeId makeClz32Symbolic(BVContext& ctx, BVNodeId val64) {
    BVNodeId v32 = ctx.make_extract(val64, 31, 0);
    BVNodeId res = ctx.make_const(32, 64);
    for (int bit = 0; bit < 32; ++bit) {
        BVNodeId bitNode = ctx.make_extract(v32, static_cast<uint32_t>(bit),
                                            static_cast<uint32_t>(bit));
        BVNodeId lz = ctx.make_const(static_cast<uint64_t>(31 - bit), 64);
        res = ctx.make_ite(bitNode, lz, res);
    }
    return res;
}

BVNodeId evalPowSymbolic(BVContext& ctx, BVNodeId base64, BVNodeId exp64, bool& ok) {
    ok = true;
    BVNodeId one64 = ctx.make_const(1, 64);
    const auto& bNode = ctx.nodes()[base64];
    const auto& eNode = ctx.nodes()[exp64];
    if (bNode.op == BVOp::Const && static_cast<int64_t>(bNode.imm) == 2) {
        BVNodeId clamped = ctx.make_binary(BVOp::And, exp64, ctx.make_const(63, 64));
        return ctx.make_binary(BVOp::Shl, one64, clamped);
    }
    if (eNode.op == BVOp::Const) {
        int64_t expVal = static_cast<int64_t>(eNode.imm);
        if (expVal >= 0 && expVal <= 16) {
            BVNodeId acc = one64;
            for (int64_t i = 0; i < expVal; ++i) {
                acc = ctx.make_binary(BVOp::Mul, acc, base64);
            }
            return acc;
        }
    }
    ok = false;
    return ctx.make_const(0, 64);
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
            globalName == "Object" || globalName == "parseInt" || globalName == "parseFloat") {
            outVal = (opVals.size() >= 3) ? opVals[2] : zero64;
            if (operands.size() >= 3) heap.propagateAlias(operands[2], resultId);
            return true;
        }
        if (globalName == "isFinite") {
            outVal = one64;
            return true;
        }
        if (globalName == "isNaN") {
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
            outVal = ctx.make_ite(ctx.make_binary(BVOp::Slt, a, zero64),
                                  ctx.make_unary(BVOp::Neg, a), a);
            return true;
        }
        if ((method == "min" || method == "max") && opVals.size() >= 3) {
            BVNodeId acc = opVals[1];
            for (size_t k = 2; k < opVals.size(); ++k) {
                BVNodeId pick = (method == "min")
                                    ? ctx.make_binary(BVOp::Slt, opVals[k], acc)
                                    : ctx.make_binary(BVOp::Slt, acc, opVals[k]);
                acc = ctx.make_ite(pick, opVals[k], acc);
            }
            outVal = acc;
            return true;
        }
        if (method == "imul" && opVals.size() >= 3) {
            BVNodeId a32 = ctx.make_extract(opVals[1], 31, 0);
            BVNodeId b32 = ctx.make_extract(opVals[2], 31, 0);
            outVal = ctx.make_sext(ctx.make_binary(BVOp::Mul, a32, b32), 32);
            return true;
        }
        if (method == "sign" && opVals.size() >= 2) {
            BVNodeId a = opVals[1];
            BVNodeId isNeg = ctx.make_binary(BVOp::Slt, a, zero64);
            BVNodeId isPos = ctx.make_binary(BVOp::Slt, zero64, a);
            outVal = ctx.make_ite(isNeg, allOnes64, ctx.make_ite(isPos, one64, zero64));
            return true;
        }
        if (method == "clz32" && opVals.size() >= 2) {
            outVal = makeClz32Symbolic(ctx, opVals[1]);
            return true;
        }
        if (method == "log2" && opVals.size() >= 2) {
            outVal = ctx.make_binary(BVOp::Sub, ctx.make_const(31, 64),
                                     makeClz32Symbolic(ctx, opVals[1]));
            return true;
        }
        if (method == "pow" && opVals.size() >= 3) {
            bool ok = false;
            outVal = evalPowSymbolic(ctx, opVals[1], opVals[2], ok);
            if (!ok && outErr) *outErr = "unsupported symbolic Math.pow";
            return true;
        }
        if ((method == "floor" || method == "ceil") && opVals.size() >= 2) {
            outVal = evalFloorOrCeilSymbolic(ctx, heap, operands[1], opVals[1], method == "ceil");
            return true;
        }
        if ((method == "trunc" || method == "round" || method == "fround") && opVals.size() >= 2) {
            outVal = opVals[1];
            return true;
        }
    }
    if (globalName == "Number" && opVals.size() >= 2) {
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
        BVNodeId ch = ctx.make_binary(BVOp::And, opVals[1], ctx.make_const(0xFFFF, 64));
        outVal = ch;
        if (outArrayLiteral) *outArrayLiteral = {ch};
        return true;
    }
    if (globalName == "Object") {
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
        outVal = one64;
        return true;
    }
    return false;
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
