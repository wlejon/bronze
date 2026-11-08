#include "check/prove_equiv_sym.h"

#include <algorithm>
#include <limits>

namespace bronze::check {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;

bool functionUsesStringOps(const il::Module& mod, const il::Function& fn) {
    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.instructions) {
            using Op = il::Op;
            if (inst.op == Op::ConcatBegin || inst.op == Op::ConcatAppend ||
                inst.op == Op::ConcatEnd || inst.op == Op::ToStr) {
                return true;
            }
            if (inst.op == Op::Box && inst.operands.empty() && inst.keyIndex < mod.keyConstants.size()) {
                const std::string& s = mod.keyConstants[inst.keyIndex];
                if (s.size() <= kMaxBoundedSeqLen &&
                    s != "undefined" && s != "object" && s != "boolean" && s != "number" &&
                    s != "string" && s != "function" && s != "symbol" && s != "bigint") {
                    return true;
                }
            }
            if (inst.op == Op::MethodCall && inst.keyIndex < mod.keyConstants.size()) {
                const std::string& m = mod.keyConstants[inst.keyIndex];
                if (m == "charCodeAt" || m == "codePointAt" || m == "charAt" ||
                    m == "substring" || m == "substr" || m == "lastIndexOf" ||
                    m == "toLowerCase" || m == "toUpperCase" ||
                    m == "fromCharCode" || m == "fromCodePoint" ||
                    m == "startsWith" || m == "endsWith") {
                    return true;
                }
            }
        }
    }
    return false;
}

uint64_t packConcreteString64(std::string_view s) {
    if (s.size() > kMaxBoundedSeqLen) {
        return hashStringLiteral(s);
    }
    uint64_t acc = (0x5ULL << 60) | (static_cast<uint64_t>(s.size() & 0xFu) << 56);
    for (size_t i = 0; i < s.size() && i < kMaxBoundedSeqLen; ++i) {
        uint64_t c7 = static_cast<unsigned char>(s[i]) & 0x7Fu;
        acc |= (c7 << (i * 7));
    }
    return acc;
}

BVNodeId packBoundedStringNode(BVContext& ctx,
                               BVNodeId len,
                               const BVNodeId elems[kMaxBoundedSeqLen]) {
    BVNodeId zero64 = ctx.make_const(0, 64);
    BVNodeId lenLow = ctx.make_binary(BVOp::And, len, ctx.make_const(0xF, 64));
    BVNodeId acc = ctx.make_binary(
        BVOp::Or, ctx.make_const(0x5ULL << 60, 64),
        ctx.make_binary(BVOp::Shl, lenLow, ctx.make_const(56, 64)));
    for (size_t i = 0; i < kMaxBoundedSeqLen; ++i) {
        BVNodeId inB = ctx.make_binary(BVOp::Ult, ctx.make_const(i, 64), len, 1);
        BVNodeId c7 = ctx.make_ite(
            inB, ctx.make_binary(BVOp::And, elems[i], ctx.make_const(0x7F, 64)), zero64);
        BVNodeId shifted = (i == 0) ? c7 : ctx.make_binary(BVOp::Shl, c7, ctx.make_const(i * 7, 64));
        acc = ctx.make_binary(BVOp::Or, acc, shifted);
    }
    return acc;
}

BVNodeId selectBoundedSeqElem(BVContext& ctx,
                              const BoundedSeq& seq,
                              BVNodeId idx,
                              BVNodeId defaultVal) {
    BVNodeId res = (defaultVal != 0) ? defaultVal : ctx.make_const(0, 64);
    for (size_t i = 0; i < kMaxBoundedSeqLen; ++i) {
        BVNodeId iConst = ctx.make_const(i, 64);
        BVNodeId hit = ctx.make_binary(
            BVOp::And, ctx.make_binary(BVOp::Eq, idx, iConst, 1),
            ctx.make_binary(BVOp::Ult, iConst, seq.len, 1), 1);
        res = ctx.make_ite(hit, seq.elems[i], res);
    }
    return res;
}

void SymbolicIlBuilder::loadBoundedSeq(SymState& state,
                                       il::ValueId valId,
                                       bool asString,
                                       BoundedSeq& outSeq) {
    outSeq.isString = asString;
    for (size_t i = 0; i < kMaxBoundedSeqLen; ++i) outSeq.elems[i] = zero64_;

    std::string path = state.heap.lookupObjPath(valId);
    if (!path.empty()) {
        path = state.heap.resolveObjPath(path);
        bool isLocal = (path.rfind("__local_", 0) == 0 || path.rfind("__fn_", 0) == 0);
        auto lenIt = state.heap.writes.find(path + ".length");
        if (lenIt != state.heap.writes.end()) {
            outSeq.len = lenIt->second;
            if (!isLocal && heapTable_.isBoundedLenPath(path + ".length")) {
                if (asString) maxStringBoundUsed_ = static_cast<uint32_t>(kMaxBoundedSeqLen);
                else maxArrayBoundUsed_ = static_cast<uint32_t>(kMaxBoundedSeqLen);
            }
        } else if (isLocal) {
            auto arrIt = state.arrayValues.find(valId);
            if (arrIt != state.arrayValues.end()) {
                size_t sz = std::min<size_t>(arrIt->second.size(), kMaxBoundedSeqLen);
                outSeq.len = ctx_.make_const(sz, 64);
                for (size_t i = 0; i < sz; ++i) outSeq.elems[i] = arrIt->second[i];
                return;
            }
            outSeq.len = zero64_;
        } else {
            outSeq.len = heapTable_.getBoundedLenVal(path + ".length", asString);
            if (asString) {
                state.heap.stringPaths.insert(path);
                maxStringBoundUsed_ = static_cast<uint32_t>(kMaxBoundedSeqLen);
            } else {
                state.heap.arrayPaths.insert(path);
                maxArrayBoundUsed_ = static_cast<uint32_t>(kMaxBoundedSeqLen);
            }
        }
        for (size_t i = 0; i < kMaxBoundedSeqLen; ++i) {
            std::string ek = path + "." + std::to_string(i);
            BVNodeId rawElem = zero64_;
            auto eIt = state.heap.writes.find(ek);
            if (eIt != state.heap.writes.end()) {
                rawElem = eIt->second;
            } else if (!isLocal) {
                rawElem = asString ? heapTable_.getCharCodeVal(ek) : heapTable_.getInitialVal(ek);
            }
            BVNodeId inB = ctx_.make_binary(BVOp::Ult, ctx_.make_const(i, 64), outSeq.len, 1);
            outSeq.elems[i] = ctx_.make_ite(inB, rawElem, zero64_);
        }
        return;
    }

    auto arrIt = state.arrayValues.find(valId);
    if (arrIt != state.arrayValues.end()) {
        size_t sz = std::min<size_t>(arrIt->second.size(), kMaxBoundedSeqLen);
        outSeq.len = ctx_.make_const(sz, 64);
        for (size_t i = 0; i < sz; ++i) outSeq.elems[i] = arrIt->second[i];
        return;
    }

    if (asString) {
        BVNodeId v = toBV64(getVal(state, valId));
        BVNodeId tag = ctx_.make_extract(v, 63, 60);
        BVNodeId isPacked = ctx_.make_binary(BVOp::Eq, tag, ctx_.make_const(0x5, 4), 1);
        BVNodeId pLen = ctx_.make_zext(ctx_.make_extract(v, 59, 56), 60);
        BVNodeId digitCh = ctx_.make_binary(
            BVOp::Add, ctx_.make_binary(BVOp::And, v, ctx_.make_const(0xF, 64)),
            ctx_.make_const(48, 64));
        outSeq.len = ctx_.make_ite(isPacked, pLen, one64_);
        for (size_t i = 0; i < kMaxBoundedSeqLen; ++i) {
            BVNodeId c7 = ctx_.make_zext(
                ctx_.make_extract(v, static_cast<uint32_t>(i * 7 + 6), static_cast<uint32_t>(i * 7)), 57);
            BVNodeId fallback = (i == 0) ? digitCh : zero64_;
            outSeq.elems[i] = ctx_.make_ite(isPacked, c7, fallback);
        }
    } else {
        outSeq.len = zero64_;
    }
}

void SymbolicIlBuilder::storeBoundedSeqToNewLocal(SymState& state,
                                                  il::ValueId resultId,
                                                  const BoundedSeq& seq) {
    BVNodeId tok = state.heap.createLocalObject(heapTable_, resultId);
    std::string p = state.heap.lookupObjPath(resultId);
    state.arrayValues.erase(resultId);
    if (!p.empty()) {
        state.heap.writes[p + ".length"] = seq.len;
        for (size_t i = 0; i < kMaxBoundedSeqLen; ++i) {
            BVNodeId inB = ctx_.make_binary(BVOp::Ult, ctx_.make_const(i, 64), seq.len, 1);
            state.heap.writes[p + "." + std::to_string(i)] = ctx_.make_ite(inB, seq.elems[i], zero64_);
        }
        if (seq.isString) {
            state.heap.stringPaths.insert(p);
        } else {
            state.heap.arrayPaths.insert(p);
        }
    }
    if (seq.isString) {
        state.values[resultId] = packBoundedStringNode(ctx_, seq.len, seq.elems);
    } else {
        state.values[resultId] = tok;
    }
}

void SymbolicIlBuilder::storeBoundedSeqInPlace(SymState& state,
                                               il::ValueId recvId,
                                               const std::string& path,
                                               const BoundedSeq& seq) {
    state.arrayValues.erase(recvId);
    if (path.empty()) return;
    std::string p = state.heap.resolveObjPath(path);
    state.heap.writes[p + ".length"] = seq.len;
    for (size_t i = 0; i < kMaxBoundedSeqLen; ++i) {
        BVNodeId inB = ctx_.make_binary(BVOp::Ult, ctx_.make_const(i, 64), seq.len, 1);
        state.heap.writes[p + "." + std::to_string(i)] = ctx_.make_ite(inB, seq.elems[i], zero64_);
    }
    if (seq.isString) state.heap.stringPaths.insert(p);
    else state.heap.arrayPaths.insert(p);
}

void SymbolicIlBuilder::evalBoxStringLiteral(const std::string& lit,
                                             il::ValueId resultId,
                                             SymState& state) {
    std::vector<BVNodeId> chars;
    for (size_t i = 0; i < lit.size(); ++i) {
        chars.push_back(ctx_.make_const(static_cast<unsigned char>(lit[i]), 64));
    }
    if (lit.size() > kMaxBoundedSeqLen) {
        std::string p = "__strlit_" + std::to_string(resultId);
        state.heap.objPaths[resultId] = p;
        state.heap.stringPaths.insert(p);
        state.values[resultId] = ctx_.make_const(hashStringLiteral(lit), 64);
        state.arrayValues[resultId] = std::move(chars);
        return;
    }
    BoundedSeq seq;
    seq.isString = true;
    seq.len = ctx_.make_const(lit.size(), 64);
    for (size_t i = 0; i < lit.size(); ++i) seq.elems[i] = chars[i];
    storeBoundedSeqToNewLocal(state, resultId, seq);
    state.arrayValues[resultId] = std::move(chars);
}

void SymbolicIlBuilder::evalStringConcatOp(il::ValueId lhsId,
                                           il::ValueId rhsId,
                                           il::ValueId resultId,
                                           SymState& state) {
    auto isLongOrNonSeq = [&](il::ValueId id) {
        auto aIt = state.arrayValues.find(id);
        if (aIt != state.arrayValues.end() && aIt->second.size() > kMaxBoundedSeqLen) return true;
        std::string p = state.heap.lookupObjPath(id);
        if (!p.empty()) return false;
        BVNodeId v = toBV64(getVal(state, id));
        if (v < ctx_.nodes().size() && ctx_.nodes()[v].op == BVOp::Const) {
            return ((ctx_.nodes()[v].imm >> 60) & 0xFULL) != 0x5ULL;
        }
        return true;
    };
    if (isLongOrNonSeq(lhsId) || isLongOrNonSeq(rhsId)) {
        BVNodeId lhs = toBV64(getVal(state, lhsId));
        BVNodeId rhs = toBV64(getVal(state, rhsId));
        BVNodeId mixed = ctx_.make_binary(BVOp::Mul, lhs, ctx_.make_const(1099511628211ULL, 64));
        state.values[resultId] = ctx_.make_binary(BVOp::Add, mixed, rhs);
        return;
    }
    BoundedSeq seqA, seqB, out;
    loadBoundedSeq(state, lhsId, /*asString=*/true, seqA);
    loadBoundedSeq(state, rhsId, /*asString=*/true, seqB);
    out.isString = true;
    BVNodeId rawLen = ctx_.make_binary(BVOp::Add, seqA.len, seqB.len);
    BVNodeId maxNode = ctx_.make_const(kMaxBoundedSeqLen, 64);
    BVNodeId ovf = ctx_.make_binary(BVOp::Ugt, rawLen, maxNode, 1);
    if (ovf < ctx_.nodes().size() &&
        !(ctx_.nodes()[ovf].op == BVOp::Const && ctx_.nodes()[ovf].imm == 0)) {
        maxStringBoundUsed_ = static_cast<uint32_t>(kMaxBoundedSeqLen);
    }
    out.len = ctx_.make_ite(ovf, maxNode, rawLen);
    for (size_t k = 0; k < kMaxBoundedSeqLen; ++k) {
        BVNodeId kConst = ctx_.make_const(k, 64);
        BVNodeId inA = ctx_.make_binary(BVOp::Ult, kConst, seqA.len, 1);
        BVNodeId inOut = ctx_.make_binary(BVOp::Ult, kConst, out.len, 1);
        BVNodeId bIdx = ctx_.make_binary(BVOp::Sub, kConst, seqA.len);
        BVNodeId bElem = selectBoundedSeqElem(ctx_, seqB, bIdx);
        out.elems[k] = ctx_.make_ite(inA, seqA.elems[k], ctx_.make_ite(inOut, bElem, zero64_));
    }
    storeBoundedSeqToNewLocal(state, resultId, out);
}

void SymbolicIlBuilder::evalArraySpreadOp(const il::Instruction& inst, SymState& state) {
    if (inst.operands.size() < 2) return;
    il::ValueId dstId = inst.operands[0];
    il::ValueId srcId = inst.operands[1];
    std::string dstPath = state.heap.lookupObjPath(dstId);
    bool srcIsStr = state.heap.isStringValue(srcId);
    BoundedSeq dst, src, out;
    loadBoundedSeq(state, dstId, /*asString=*/false, dst);
    loadBoundedSeq(state, srcId, srcIsStr, src);
    out.isString = false;
    BVNodeId rawLen = ctx_.make_binary(BVOp::Add, dst.len, src.len);
    BVNodeId maxNode = ctx_.make_const(kMaxBoundedSeqLen, 64);
    BVNodeId ovf = ctx_.make_binary(BVOp::Ugt, rawLen, maxNode, 1);
    out.len = ctx_.make_ite(ovf, maxNode, rawLen);
    for (size_t k = 0; k < kMaxBoundedSeqLen; ++k) {
        BVNodeId kConst = ctx_.make_const(k, 64);
        BVNodeId inDst = ctx_.make_binary(BVOp::Ult, kConst, dst.len, 1);
        BVNodeId inOut = ctx_.make_binary(BVOp::Ult, kConst, out.len, 1);
        BVNodeId srcIdx = ctx_.make_binary(BVOp::Sub, kConst, dst.len);
        BVNodeId srcElem = selectBoundedSeqElem(ctx_, src, srcIdx);
        out.elems[k] = ctx_.make_ite(inDst, dst.elems[k], ctx_.make_ite(inOut, srcElem, zero64_));
    }
    storeBoundedSeqInPlace(state, dstId, dstPath, out);
}

SymEvalOutcome SymbolicIlBuilder::evalCallSpreadOp(const il::Instruction& inst,
                                                   bool isMethodSpread,
                                                   SymState& state) {
    std::string gName;
    std::string method;
    il::ValueId spreadArrId = il::kNoValue;
    if (isMethodSpread) {
        if (inst.operands.size() >= 2 && inst.keyIndex < mod_.keyConstants.size()) {
            method = mod_.keyConstants[inst.keyIndex];
            if (state.globalRefs.count(inst.operands[0])) {
                gName = state.globalRefs[inst.operands[0]];
            }
            spreadArrId = inst.operands[1];
        }
    } else if (inst.operands.size() >= 3 && state.globalRefs.count(inst.operands[0])) {
        const std::string& full = state.globalRefs[inst.operands[0]];
        auto dot = full.find('.');
        if (dot != std::string::npos) {
            gName = full.substr(0, dot);
            method = full.substr(dot + 1);
        } else {
            gName = full;
        }
        spreadArrId = inst.operands[2];
    }
    if (gName == "String" && (method == "fromCharCode" || method == "fromCodePoint") &&
        spreadArrId != il::kNoValue) {
        BoundedSeq seq;
        loadBoundedSeq(state, spreadArrId, /*asString=*/false, seq);
        seq.isString = true;
        for (size_t i = 0; i < kMaxBoundedSeqLen; ++i) {
            seq.elems[i] = ctx_.make_binary(BVOp::And, seq.elems[i], ctx_.make_const(0xFFFF, 64));
        }
        storeBoundedSeqToNewLocal(state, inst.result, seq);
        return {true, {}, false, zero64_, {}, {}, zero1_};
    }
    if (inModuleInit_) {
        if (inst.result != il::kNoValue) state.values[inst.result] = zero64_;
        return {true, {}, false, zero64_, {}, {}, zero1_};
    }
    return {false, "unsupported spread call", false, zero64_, {}, {}, zero1_};
}

namespace {

BVNodeId normalizeSliceIndex(BVContext& ctx, BVNodeId idx, BVNodeId len) {
    BVNodeId zero64 = ctx.make_const(0, 64);
    BVNodeId isNeg = ctx.make_binary(BVOp::Slt, idx, zero64, 1);
    BVNodeId rel = ctx.make_binary(BVOp::Add, len, idx);
    BVNodeId negClamped = ctx.make_ite(ctx.make_binary(BVOp::Slt, rel, zero64, 1), zero64, rel);
    BVNodeId posClamped = ctx.make_ite(ctx.make_binary(BVOp::Slt, len, idx, 1), len, idx);
    return ctx.make_ite(isNeg, negClamped, posClamped);
}

BVNodeId normalizeSubstringIndex(BVContext& ctx, BVNodeId idx, BVNodeId len) {
    BVNodeId zero64 = ctx.make_const(0, 64);
    BVNodeId nonNeg = ctx.make_ite(ctx.make_binary(BVOp::Slt, idx, zero64, 1), zero64, idx);
    return ctx.make_ite(ctx.make_binary(BVOp::Slt, len, nonNeg, 1), len, nonNeg);
}

}  // namespace

bool SymbolicIlBuilder::tryEvalSeqMethodCallOp(const std::string& method,
                                               const il::Instruction& inst,
                                               SymState& state,
                                               SymEvalOutcome& outOutcome) {
    if (inst.operands.empty()) return false;
    il::ValueId recvId = inst.operands[0];
    std::string recvPath = state.heap.lookupObjPath(recvId);

    if ((method == "charCodeAt" || method == "codePointAt" || method == "charAt") &&
        inst.operands.size() >= 2) {
        BVNodeId idx = toSInt32In64(getVal(state, inst.operands[1]));
        BoundedSeq seq;
        loadBoundedSeq(state, recvId, /*asString=*/true, seq);
        if (method == "charAt") {
            BoundedSeq out;
            out.isString = true;
            BVNodeId inB = ctx_.make_binary(
                BVOp::And, ctx_.make_binary(BVOp::Sge, idx, zero64_, 1),
                ctx_.make_binary(BVOp::Slt, idx, seq.len, 1), 1);
            out.len = ctx_.make_ite(inB, one64_, zero64_);
            out.elems[0] = ctx_.make_ite(inB, selectBoundedSeqElem(ctx_, seq, idx), zero64_);
            storeBoundedSeqToNewLocal(state, inst.result, out);
        } else {
            BVNodeId oobVal = floatMode_
                ? makeF64ConstNode(ctx_, std::numeric_limits<double>::quiet_NaN())
                : zero64_;
            BVNodeId ch = selectBoundedSeqElem(ctx_, seq, idx, oobVal);
            if (floatMode_) {
                BVNodeId inB = ctx_.make_binary(
                    BVOp::And, ctx_.make_binary(BVOp::Sge, idx, zero64_, 1),
                    ctx_.make_binary(BVOp::Slt, idx, seq.len, 1), 1);
                BVNodeId asF64 = ctx_.make_unary(BVOp::SiToFp, ctx_.make_extract(ch, 31, 0), 64);
                state.values[inst.result] = ctx_.make_ite(inB, asF64, oobVal);
            } else {
                state.values[inst.result] = ch;
            }
        }
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    if (method == "toLowerCase" || method == "toUpperCase" ||
        method == "toLocaleLowerCase" || method == "toLocaleUpperCase") {
        bool isLower = (method == "toLowerCase" || method == "toLocaleLowerCase");
        BoundedSeq seq, out;
        loadBoundedSeq(state, recvId, /*asString=*/true, seq);
        out.isString = true;
        out.len = seq.len;
        uint64_t lo = isLower ? 65 : 97;
        uint64_t hi = isLower ? 90 : 122;
        for (size_t k = 0; k < kMaxBoundedSeqLen; ++k) {
            BVNodeId c = seq.elems[k];
            BVNodeId inRange = ctx_.make_binary(
                BVOp::And, ctx_.make_binary(BVOp::Uge, c, ctx_.make_const(lo, 64), 1),
                ctx_.make_binary(BVOp::Ule, c, ctx_.make_const(hi, 64), 1), 1);
            BVNodeId shifted = isLower
                ? ctx_.make_binary(BVOp::Add, c, ctx_.make_const(32, 64))
                : ctx_.make_binary(BVOp::Sub, c, ctx_.make_const(32, 64));
            out.elems[k] = ctx_.make_ite(inRange, shifted, c);
        }
        storeBoundedSeqToNewLocal(state, inst.result, out);
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    if (method == "slice" || method == "subarray" || method == "substring") {
        bool isSubstring = (method == "substring");
        bool asString = isSubstring || state.heap.isStringValue(recvId) ||
                        heapTable_.isStringRoot(recvPath) || heapTable_.preferStringSeq();
        BoundedSeq seq, out;
        loadBoundedSeq(state, recvId, asString, seq);
        out.isString = asString;
        BVNodeId rawStart = (inst.operands.size() >= 2 && !state.undefValues.count(inst.operands[1]))
                                ? toSInt32In64(getVal(state, inst.operands[1])) : zero64_;
        BVNodeId rawEnd = (inst.operands.size() >= 3 && !state.undefValues.count(inst.operands[2]))
                              ? toSInt32In64(getVal(state, inst.operands[2])) : seq.len;
        BVNodeId sIdx = 0, eIdx = 0;
        if (isSubstring) {
            BVNodeId a = normalizeSubstringIndex(ctx_, rawStart, seq.len);
            BVNodeId b = normalizeSubstringIndex(ctx_, rawEnd, seq.len);
            BVNodeId swap = ctx_.make_binary(BVOp::Slt, b, a, 1);
            sIdx = ctx_.make_ite(swap, b, a);
            eIdx = ctx_.make_ite(swap, a, b);
        } else {
            sIdx = normalizeSliceIndex(ctx_, rawStart, seq.len);
            eIdx = normalizeSliceIndex(ctx_, rawEnd, seq.len);
        }
        BVNodeId validSpan = ctx_.make_binary(BVOp::Slt, sIdx, eIdx, 1);
        out.len = ctx_.make_ite(validSpan, ctx_.make_binary(BVOp::Sub, eIdx, sIdx), zero64_);
        for (size_t k = 0; k < kMaxBoundedSeqLen; ++k) {
            BVNodeId kConst = ctx_.make_const(k, 64);
            BVNodeId inB = ctx_.make_binary(BVOp::Ult, kConst, out.len, 1);
            BVNodeId srcIdx = ctx_.make_binary(BVOp::Add, sIdx, kConst);
            out.elems[k] = ctx_.make_ite(inB, selectBoundedSeqElem(ctx_, seq, srcIdx), zero64_);
        }
        storeBoundedSeqToNewLocal(state, inst.result, out);
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    if (method == "indexOf" || method == "lastIndexOf") {
        if (inst.operands.size() < 2) return false;
        bool isLast = (method == "lastIndexOf");
        il::ValueId needleId = inst.operands[1];
        bool asString = state.heap.isStringValue(recvId) || state.heap.isStringValue(needleId) ||
                        heapTable_.isStringRoot(recvPath) || heapTable_.preferStringSeq();
        BVNodeId defFrom = isLast ? ctx_.make_const(kMaxBoundedSeqLen, 64) : zero64_;
        BVNodeId rawFrom = (inst.operands.size() >= 3 && !state.undefValues.count(inst.operands[2]))
                               ? toSInt32In64(getVal(state, inst.operands[2])) : defFrom;
        BVNodeId fromIdx = ctx_.make_ite(ctx_.make_binary(BVOp::Slt, rawFrom, zero64_, 1), zero64_, rawFrom);
        BVNodeId res = allOnes64_;
        if (asString) {
            BoundedSeq hay, ndl;
            loadBoundedSeq(state, recvId, /*asString=*/true, hay);
            loadBoundedSeq(state, needleId, /*asString=*/true, ndl);
            int startP = isLast ? 0 : static_cast<int>(kMaxBoundedSeqLen);
            int endP = isLast ? (static_cast<int>(kMaxBoundedSeqLen) + 1) : -1;
            int stepP = isLast ? 1 : -1;
            for (int p = startP; p != endP; p += stepP) {
                BVNodeId pConst = ctx_.make_const(static_cast<uint64_t>(p), 64);
                BVNodeId fromCmp = isLast
                    ? ctx_.make_binary(BVOp::Sle, pConst, fromIdx, 1)
                    : ctx_.make_binary(BVOp::Sge, pConst, fromIdx, 1);
                BVNodeId fits = ctx_.make_binary(
                    BVOp::And, fromCmp,
                    ctx_.make_binary(BVOp::Ule, ctx_.make_binary(BVOp::Add, pConst, ndl.len), hay.len, 1), 1);
                for (size_t j = 0; j < kMaxBoundedSeqLen; ++j) {
                    if (static_cast<size_t>(p) + j >= kMaxBoundedSeqLen) break;
                    BVNodeId jNeeded = ctx_.make_binary(BVOp::Ult, ctx_.make_const(j, 64), ndl.len, 1);
                    BVNodeId charEq = ctx_.make_binary(BVOp::Eq, hay.elems[static_cast<size_t>(p) + j], ndl.elems[j], 1);
                    fits = ctx_.make_binary(BVOp::And, fits,
                                            ctx_.make_binary(BVOp::Or, ctx_.make_unary(BVOp::Not, jNeeded, 1), charEq, 1), 1);
                }
                res = ctx_.make_ite(fits, pConst, res);
            }
        } else {
            BoundedSeq arr;
            loadBoundedSeq(state, recvId, /*asString=*/false, arr);
            BVNodeId target = toBV64(getVal(state, needleId));
            int startP = isLast ? 0 : (static_cast<int>(kMaxBoundedSeqLen) - 1);
            int endP = isLast ? static_cast<int>(kMaxBoundedSeqLen) : -1;
            int stepP = isLast ? 1 : -1;
            for (int p = startP; p != endP; p += stepP) {
                BVNodeId pConst = ctx_.make_const(static_cast<uint64_t>(p), 64);
                BVNodeId fromCmp = isLast
                    ? ctx_.make_binary(BVOp::Sle, pConst, fromIdx, 1)
                    : ctx_.make_binary(BVOp::Sge, pConst, fromIdx, 1);
                BVNodeId inRange = ctx_.make_binary(
                    BVOp::And, fromCmp,
                    ctx_.make_binary(BVOp::Ult, pConst, arr.len, 1), 1);
                BVNodeId matchP = ctx_.make_binary(BVOp::And, inRange,
                                                   ctx_.make_binary(BVOp::Eq, arr.elems[p], target, 1), 1);
                res = ctx_.make_ite(matchP, pConst, res);
            }
        }
        state.values[inst.result] = floatMode_
            ? ctx_.make_unary(BVOp::SiToFp, ctx_.make_extract(res, 31, 0), 64)
            : res;
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    if (method == "substr" && inst.operands.size() >= 2) {
        BoundedSeq seq, out;
        loadBoundedSeq(state, recvId, /*asString=*/true, seq);
        out.isString = true;
        BVNodeId rawStart = !state.undefValues.count(inst.operands[1])
                                ? toSInt32In64(getVal(state, inst.operands[1])) : zero64_;
        BVNodeId sIdx = normalizeSliceIndex(ctx_, rawStart, seq.len);
        BVNodeId rem = ctx_.make_binary(BVOp::Sub, seq.len, sIdx);
        BVNodeId cnt = rem;
        if (inst.operands.size() >= 3 && !state.undefValues.count(inst.operands[2])) {
            BVNodeId rawLen = toSInt32In64(getVal(state, inst.operands[2]));
            BVNodeId nonNeg = ctx_.make_ite(ctx_.make_binary(BVOp::Slt, rawLen, zero64_, 1), zero64_, rawLen);
            cnt = ctx_.make_ite(ctx_.make_binary(BVOp::Slt, rem, nonNeg, 1), rem, nonNeg);
        }
        out.len = cnt;
        for (size_t k = 0; k < kMaxBoundedSeqLen; ++k) {
            BVNodeId kConst = ctx_.make_const(k, 64);
            BVNodeId inB = ctx_.make_binary(BVOp::Ult, kConst, out.len, 1);
            BVNodeId srcIdx = ctx_.make_binary(BVOp::Add, sIdx, kConst);
            out.elems[k] = ctx_.make_ite(inB, selectBoundedSeqElem(ctx_, seq, srcIdx), zero64_);
        }
        storeBoundedSeqToNewLocal(state, inst.result, out);
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    if (method == "concat" && inst.operands.size() >= 2) {
        evalStringConcatOp(recvId, inst.operands[1], inst.result, state);
        for (size_t k = 2; k < inst.operands.size(); ++k) {
            evalStringConcatOp(inst.result, inst.operands[k], inst.result, state);
        }
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    if (method == "push" && inst.operands.size() >= 2) {
        BoundedSeq seq;
        loadBoundedSeq(state, recvId, /*asString=*/false, seq);
        BVNodeId elem = toBV64(getVal(state, inst.operands[1]));
        BVNodeId maxNode = ctx_.make_const(kMaxBoundedSeqLen, 64);
        BVNodeId ovf = ctx_.make_binary(BVOp::Uge, seq.len, maxNode, 1);
        if (ovf < ctx_.nodes().size() &&
            !(ctx_.nodes()[ovf].op == BVOp::Const && ctx_.nodes()[ovf].imm == 0)) {
            state.exceededCond = (state.exceededCond == 0 || state.exceededCond == zero1_)
                                     ? ovf
                                     : ctx_.make_binary(BVOp::Or, state.exceededCond, ovf, 1);
            maxArrayBoundUsed_ = static_cast<uint32_t>(kMaxBoundedSeqLen);
        }
        for (size_t k = 0; k < kMaxBoundedSeqLen; ++k) {
            BVNodeId eqK = ctx_.make_binary(BVOp::Eq, seq.len, ctx_.make_const(k, 64), 1);
            seq.elems[k] = ctx_.make_ite(eqK, elem, seq.elems[k]);
        }
        seq.len = ctx_.make_ite(ovf, maxNode, ctx_.make_binary(BVOp::Add, seq.len, one64_));
        storeBoundedSeqInPlace(state, recvId, recvPath, seq);
        state.values[inst.result] = seq.len;
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    if (method == "pop") {
        BoundedSeq seq;
        loadBoundedSeq(state, recvId, /*asString=*/false, seq);
        BVNodeId isEmpty = ctx_.make_binary(BVOp::Eq, seq.len, zero64_, 1);
        BVNodeId newLen = ctx_.make_ite(isEmpty, zero64_, ctx_.make_binary(BVOp::Sub, seq.len, one64_));
        BVNodeId popped = ctx_.make_ite(isEmpty, zero64_, selectBoundedSeqElem(ctx_, seq, newLen));
        seq.len = newLen;
        storeBoundedSeqInPlace(state, recvId, recvPath, seq);
        state.values[inst.result] = popped;
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    if (method == "shift") {
        BoundedSeq seq;
        loadBoundedSeq(state, recvId, /*asString=*/false, seq);
        BVNodeId isEmpty = ctx_.make_binary(BVOp::Eq, seq.len, zero64_, 1);
        BVNodeId first = ctx_.make_ite(isEmpty, zero64_, seq.elems[0]);
        BVNodeId newLen = ctx_.make_ite(isEmpty, zero64_, ctx_.make_binary(BVOp::Sub, seq.len, one64_));
        for (size_t k = 0; k + 1 < kMaxBoundedSeqLen; ++k) {
            BVNodeId inB = ctx_.make_binary(BVOp::Ult, ctx_.make_const(k, 64), newLen, 1);
            seq.elems[k] = ctx_.make_ite(inB, seq.elems[k + 1], zero64_);
        }
        seq.elems[kMaxBoundedSeqLen - 1] = zero64_;
        seq.len = newLen;
        storeBoundedSeqInPlace(state, recvId, recvPath, seq);
        state.values[inst.result] = first;
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    if (method == "unshift" && inst.operands.size() >= 2) {
        BoundedSeq seq;
        loadBoundedSeq(state, recvId, /*asString=*/false, seq);
        BVNodeId elem = toBV64(getVal(state, inst.operands[1]));
        BVNodeId maxNode = ctx_.make_const(kMaxBoundedSeqLen, 64);
        BVNodeId ovf = ctx_.make_binary(BVOp::Uge, seq.len, maxNode, 1);
        if (ovf < ctx_.nodes().size() &&
            !(ctx_.nodes()[ovf].op == BVOp::Const && ctx_.nodes()[ovf].imm == 0)) {
            state.exceededCond = (state.exceededCond == 0 || state.exceededCond == zero1_)
                                     ? ovf
                                     : ctx_.make_binary(BVOp::Or, state.exceededCond, ovf, 1);
            maxArrayBoundUsed_ = static_cast<uint32_t>(kMaxBoundedSeqLen);
        }
        BVNodeId newLen = ctx_.make_ite(ovf, maxNode, ctx_.make_binary(BVOp::Add, seq.len, one64_));
        for (size_t k = kMaxBoundedSeqLen - 1; k >= 1; --k) {
            BVNodeId inB = ctx_.make_binary(BVOp::Ult, ctx_.make_const(k, 64), newLen, 1);
            seq.elems[k] = ctx_.make_ite(inB, seq.elems[k - 1], zero64_);
        }
        seq.elems[0] = elem;
        seq.len = newLen;
        storeBoundedSeqInPlace(state, recvId, recvPath, seq);
        state.values[inst.result] = newLen;
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    if (method == "join") {
        BoundedSeq arr, sep, out;
        loadBoundedSeq(state, recvId, /*asString=*/false, arr);
        out.isString = true;
        if (inst.operands.size() < 2 || state.undefValues.count(inst.operands[1])) {
            sep.isString = true;
            sep.len = one64_;
            sep.elems[0] = ctx_.make_const(44, 64);  // ','
        } else {
            loadBoundedSeq(state, inst.operands[1], /*asString=*/true, sep);
        }
        BVNodeId elemChars[kMaxBoundedSeqLen];
        for (size_t i = 0; i < kMaxBoundedSeqLen; ++i) {
            BVNodeId v = arr.elems[i];
            BVNodeId isDigit = ctx_.make_binary(BVOp::Ule, v, ctx_.make_const(9, 64), 1);
            BVNodeId asDigit = ctx_.make_binary(BVOp::Add, v, ctx_.make_const(48, 64));
            BVNodeId low7 = ctx_.make_binary(BVOp::And, v, ctx_.make_const(0x7F, 64));
            elemChars[i] = ctx_.make_ite(isDigit, asDigit, low7);
        }
        BVNodeId sepEmpty = ctx_.make_binary(BVOp::Eq, sep.len, zero64_, 1);
        BVNodeId emptyJoinLen = arr.len;
        BVNodeId sep1Ovf = ctx_.make_binary(BVOp::Ugt, arr.len, ctx_.make_const(4, 64), 1);
        BVNodeId needExceed = ctx_.make_binary(BVOp::And, ctx_.make_unary(BVOp::Not, sepEmpty, 1), sep1Ovf, 1);
        if (needExceed < ctx_.nodes().size() &&
            !(ctx_.nodes()[needExceed].op == BVOp::Const && ctx_.nodes()[needExceed].imm == 0)) {
            state.exceededCond = (state.exceededCond == 0 || state.exceededCond == zero1_)
                                     ? needExceed
                                     : ctx_.make_binary(BVOp::Or, state.exceededCond, needExceed, 1);
            maxArrayBoundUsed_ = static_cast<uint32_t>(kMaxBoundedSeqLen);
        }
        BVNodeId sep1Len = ctx_.make_ite(
            ctx_.make_binary(BVOp::Eq, arr.len, zero64_, 1), zero64_,
            ctx_.make_binary(BVOp::Sub, ctx_.make_binary(BVOp::Mul, arr.len, ctx_.make_const(2, 64)), one64_));
        out.len = ctx_.make_ite(sepEmpty, emptyJoinLen, sep1Len);
        for (size_t k = 0; k < kMaxBoundedSeqLen; ++k) {
            BVNodeId kConst = ctx_.make_const(k, 64);
            BVNodeId inOut = ctx_.make_binary(BVOp::Ult, kConst, out.len, 1);
            BVNodeId sep1Val = ((k % 2) == 0) ? elemChars[k / 2] : sep.elems[0];
            BVNodeId valK = ctx_.make_ite(sepEmpty, elemChars[k], sep1Val);
            out.elems[k] = ctx_.make_ite(inOut, valK, zero64_);
        }
        storeBoundedSeqToNewLocal(state, inst.result, out);
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    if (method == "reverse") {
        BoundedSeq seq, out;
        loadBoundedSeq(state, recvId, /*asString=*/false, seq);
        out.isString = false;
        out.len = seq.len;
        for (size_t k = 0; k < kMaxBoundedSeqLen; ++k) {
            BVNodeId kConst = ctx_.make_const(k, 64);
            BVNodeId inB = ctx_.make_binary(BVOp::Ult, kConst, seq.len, 1);
            BVNodeId revIdx = ctx_.make_binary(BVOp::Sub, ctx_.make_binary(BVOp::Sub, seq.len, one64_), kConst);
            out.elems[k] = ctx_.make_ite(inB, selectBoundedSeqElem(ctx_, seq, revIdx), zero64_);
        }
        storeBoundedSeqInPlace(state, recvId, recvPath, out);
        state.values[inst.result] = toBV64(getVal(state, recvId));
        state.heap.propagateAlias(recvId, inst.result);
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    if (method == "splice" && inst.operands.size() >= 2) {
        BoundedSeq seq, rem, nextSeq;
        loadBoundedSeq(state, recvId, /*asString=*/false, seq);
        rem.isString = false;
        nextSeq.isString = false;
        BVNodeId rawStart = toSInt32In64(getVal(state, inst.operands[1]));
        BVNodeId sIdx = normalizeSliceIndex(ctx_, rawStart, seq.len);
        BVNodeId tailLen = ctx_.make_binary(BVOp::Sub, seq.len, sIdx);
        BVNodeId delCnt = tailLen;
        if (inst.operands.size() >= 3 && !state.undefValues.count(inst.operands[2])) {
            BVNodeId rawDel = toSInt32In64(getVal(state, inst.operands[2]));
            BVNodeId nonNeg = ctx_.make_ite(ctx_.make_binary(BVOp::Slt, rawDel, zero64_, 1), zero64_, rawDel);
            delCnt = ctx_.make_ite(ctx_.make_binary(BVOp::Slt, tailLen, nonNeg, 1), tailLen, nonNeg);
        }
        rem.len = delCnt;
        for (size_t k = 0; k < kMaxBoundedSeqLen; ++k) {
            BVNodeId kConst = ctx_.make_const(k, 64);
            BVNodeId inRem = ctx_.make_binary(BVOp::Ult, kConst, delCnt, 1);
            rem.elems[k] = ctx_.make_ite(inRem, selectBoundedSeqElem(ctx_, seq, ctx_.make_binary(BVOp::Add, sIdx, kConst)), zero64_);
        }
        bool hasIns = (inst.operands.size() >= 4);
        BVNodeId insVal = hasIns ? toBV64(getVal(state, inst.operands[3])) : zero64_;
        BVNodeId insCntNode = hasIns ? one64_ : zero64_;
        BVNodeId rawNewLen = ctx_.make_binary(BVOp::Add, ctx_.make_binary(BVOp::Sub, seq.len, delCnt), insCntNode);
        BVNodeId maxNode = ctx_.make_const(kMaxBoundedSeqLen, 64);
        BVNodeId ovf = ctx_.make_binary(BVOp::Ugt, rawNewLen, maxNode, 1);
        if (ovf < ctx_.nodes().size() &&
            !(ctx_.nodes()[ovf].op == BVOp::Const && ctx_.nodes()[ovf].imm == 0)) {
            state.exceededCond = (state.exceededCond == 0 || state.exceededCond == zero1_)
                                     ? ovf : ctx_.make_binary(BVOp::Or, state.exceededCond, ovf, 1);
            maxArrayBoundUsed_ = static_cast<uint32_t>(kMaxBoundedSeqLen);
        }
        nextSeq.len = ctx_.make_ite(ovf, maxNode, rawNewLen);
        for (size_t k = 0; k < kMaxBoundedSeqLen; ++k) {
            BVNodeId kConst = ctx_.make_const(k, 64);
            BVNodeId inOut = ctx_.make_binary(BVOp::Ult, kConst, nextSeq.len, 1);
            BVNodeId beforeStart = ctx_.make_binary(BVOp::Ult, kConst, sIdx, 1);
            BVNodeId atStart = ctx_.make_binary(BVOp::Eq, kConst, sIdx, 1);
            BVNodeId shiftIdx = ctx_.make_binary(BVOp::Add, ctx_.make_binary(BVOp::Sub, kConst, insCntNode), delCnt);
            BVNodeId afterVal = selectBoundedSeqElem(ctx_, seq, shiftIdx);
            BVNodeId midVal = hasIns ? ctx_.make_ite(atStart, insVal, afterVal) : afterVal;
            BVNodeId elemK = ctx_.make_ite(beforeStart, seq.elems[k], midVal);
            nextSeq.elems[k] = ctx_.make_ite(inOut, elemK, zero64_);
        }
        storeBoundedSeqInPlace(state, recvId, recvPath, nextSeq);
        storeBoundedSeqToNewLocal(state, inst.result, rem);
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    if (method == "set" && inst.operands.size() == 3) {
        BoundedSeq dst, src;
        loadBoundedSeq(state, recvId, /*asString=*/false, dst);
        loadBoundedSeq(state, inst.operands[1], /*asString=*/false, src);
        BVNodeId offset = toSInt32In64(getVal(state, inst.operands[2]));
        for (size_t k = 0; k < kMaxBoundedSeqLen; ++k) {
            BVNodeId kConst = ctx_.make_const(k, 64);
            BVNodeId rel = ctx_.make_binary(BVOp::Sub, kConst, offset);
            BVNodeId inSrc = ctx_.make_binary(
                BVOp::And, ctx_.make_binary(BVOp::Sge, kConst, offset, 1),
                ctx_.make_binary(BVOp::Ult, rel, src.len, 1), 1);
            dst.elems[k] = ctx_.make_ite(inSrc, selectBoundedSeqElem(ctx_, src, rel), dst.elems[k]);
        }
        storeBoundedSeqInPlace(state, recvId, recvPath, dst);
        state.values[inst.result] = zero64_;
        outOutcome = {true, {}, false, zero64_, {}, {}, zero1_};
        return true;
    }

    return false;
}

void SymbolicIlBuilder::canonicalizeSeqReturn(il::ValueId retOpId,
                                              SymState& state,
                                              std::string& retPath,
                                              BVNodeId& ret) {
    if (retPath.empty()) return;
    std::string resolved = state.heap.resolveObjPath(retPath);
    if (state.heap.isStringPath(resolved) || heapTable_.isStringRoot(resolved) ||
        (resolved.rfind("arg_", 0) == 0 && heapTable_.preferStringSeq())) {
        BoundedSeq seq;
        loadBoundedSeq(state, retOpId, /*asString=*/true, seq);
        ret = packBoundedStringNode(ctx_, seq.len, seq.elems);
        retPath.clear();
        return;
    }
    if (resolved.rfind("arg_", 0) == 0 &&
        (state.heap.arrayPaths.count(resolved) || heapTable_.isArrayRoot(resolved))) {
        BoundedSeq seq;
        loadBoundedSeq(state, retOpId, /*asString=*/false, seq);
        state.heap.writes["__ret.length"] = seq.len;
        for (size_t i = 0; i < kMaxBoundedSeqLen; ++i) {
            state.heap.writes["__ret." + std::to_string(i)] = seq.elems[i];
        }
        retPath = "__ret";
        ret = heapTable_.getObjectToken("__ret");
    }
}

bool evalIlFunctionSymbolic(brass::verify::BVContext& ctx,
                            const il::Module& mod,
                            const il::Function& fn,
                            const std::vector<brass::verify::BVNodeId>& argNodes,
                            bool strictJs32Bitwise,
                            brass::verify::BVNodeId* outRetNode,
                            std::string* outReason,
                            SymbolicHeapTable* sharedHeapTable,
                            SymbolicHeapState* outFinalHeap,
                            bool* outHitLoopUnrollBound,
                            brass::verify::BVNodeId* outThrewNode,
                            brass::verify::BVNodeId* outExceededBoundNode,
                            ProofBounds* outBounds) {
    SymbolicIlBuilder builder(ctx, mod, strictJs32Bitwise, sharedHeapTable);
    auto out = builder.evalFunction(fn, argNodes);
    if (outHitLoopUnrollBound) *outHitLoopUnrollBound = builder.hitLoopUnrollBound();
    if (outBounds) *outBounds = builder.boundsUsed();
    if (!out.supported || !out.hasReturn) {
        if (outReason) *outReason = out.unsupportedReason;
        return false;
    }
    if (outRetNode) *outRetNode = out.returnVal;
    if (outFinalHeap) *outFinalHeap = std::move(out.finalHeap);
    if (outThrewNode) *outThrewNode = (out.threwNode != 0) ? out.threwNode : ctx.make_const(0, 1);
    if (outExceededBoundNode) *outExceededBoundNode = (out.exceededBoundNode != 0) ? out.exceededBoundNode : ctx.make_const(0, 1);
    return true;
}

}  // namespace bronze::check
