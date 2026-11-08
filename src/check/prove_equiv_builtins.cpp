#include "check/prove_equiv_heap.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_set>

namespace bronze::check {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;

namespace {

enum class DivUseKind : uint8_t { TruncatedToInt, FloorOrCeilOnly, RawFloat };

DivUseKind classifyDivResultUsage(const il::Module& mod,
                                  const il::Function& fn,
                                  const il::Block& startBlk,
                                  size_t divInstIdx) {
    il::ValueId rootRes = startBlk.instructions[divInstIdx].result;
    if (rootRes == il::kNoValue) return DivUseKind::TruncatedToInt;
    std::unordered_set<il::ValueId> tracked{rootRes};
    bool sawFloorOrCeil = false;
    bool sawTrunc = false;

    const il::Block* curBlk = &startBlk;
    size_t startIdx = divInstIdx + 1;
    for (int hop = 0; hop < 4 && curBlk != nullptr; ++hop) {
        const il::Block* nextBlk = nullptr;
        for (size_t j = startIdx; j < curBlk->instructions.size(); ++j) {
            const auto& u = curBlk->instructions[j];
            using Op = il::Op;
            if (u.op == Op::Jump && u.target.block < fn.blocks.size()) {
                const auto& tb = fn.blocks[u.target.block];
                for (size_t k = 0; k < tb.params.size() && k < u.target.args.size(); ++k) {
                    if (tracked.count(u.target.args[k])) {
                        tracked.insert(tb.params[k].id);
                        nextBlk = &tb;
                    }
                }
                continue;
            }
            bool usesTracked = false;
            for (il::ValueId op : u.operands) {
                if (tracked.count(op)) {
                    usesTracked = true;
                    break;
                }
            }
            if (!usesTracked) continue;

            if (u.op == Op::KeepAlive || u.op == Op::PinGuard ||
                u.op == Op::CensusRecord || u.op == Op::FeedbackRecord) {
                continue;
            }
            if (u.op == Op::Box || u.op == Op::Unbox || u.op == Op::ToNumeric ||
                u.op == Op::Add || u.op == Op::Sub) {
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
            if (u.op == Op::DynamicCall && u.operands.size() == 3 && tracked.count(u.operands[2])) {
                sawFloorOrCeil = true;
                continue;
            }
            return DivUseKind::RawFloat;
        }
        curBlk = nextBlk;
        startIdx = 0;
    }

    if (sawFloorOrCeil && !sawTrunc) return DivUseKind::FloorOrCeilOnly;
    if (sawTrunc && !sawFloorOrCeil) return DivUseKind::TruncatedToInt;
    return DivUseKind::RawFloat;
}

BVNodeId makeConstAwareDivOrRem(BVContext& ctx,
                                BVOp op,
                                BVNodeId a,
                                BVNodeId b,
                                int32_t domainMin = INT32_MIN,
                                int32_t domainMax = INT32_MAX) {
    const auto& bNode = ctx.nodes()[b];
    if (bNode.op == BVOp::Const) {
        int64_t divisor = static_cast<int64_t>(bNode.imm);
        if (divisor > 0 && divisor <= INT32_MAX &&
            (static_cast<uint64_t>(divisor) & (static_cast<uint64_t>(divisor) - 1)) != 0) {
            auto aIv = getSignedInterval64(ctx, a, domainMin, domainMax);
            if (aIv.smin >= INT32_MIN && aIv.smax <= INT32_MAX) {
                BVNodeId a32 = ctx.make_extract(a, 31, 0);
                BVNodeId b32 = ctx.make_const(static_cast<uint64_t>(divisor), 32);
                BVNodeId r32 = brass::verify::expand_const_div_rem_bv(ctx, op, a32, b32, 32);
                if (r32 != UINT32_MAX) return ctx.make_sext(r32, 32);
            }
        }
    }
    return ctx.make_binary(op, a, b);
}

}  // namespace

brass::verify::BVInterval getSignedInterval64(const BVContext& ctx,
                                              BVNodeId id,
                                              int32_t domainMin,
                                              int32_t domainMax,
                                              int /*depth*/,
                                              const std::unordered_map<BVNodeId, brass::verify::BVInterval>* refined) {
    std::unordered_map<BVNodeId, brass::verify::BVInterval> memo;
    if (refined) {
        memo = *refined;
    }
    auto safeAdd = [](int64_t x, int64_t y, int64_t& out) -> bool {
        if ((y > 0 && x > INT64_MAX - y) || (y < 0 && x < INT64_MIN - y)) return false;
        out = x + y;
        return true;
    };
    auto safeSub = [](int64_t x, int64_t y, int64_t& out) -> bool {
        if ((y < 0 && x > INT64_MAX + y) || (y > 0 && x < INT64_MIN + y)) return false;
        out = x - y;
        return true;
    };
    auto safeMul = [](int64_t x, int64_t y, int64_t& out) -> bool {
        if (x == 0 || y == 0) { out = 0; return true; }
        if (x == INT64_MIN || y == INT64_MIN) return false;
        if (std::abs(x) > INT64_MAX / std::abs(y)) return false;
        out = x * y;
        return true;
    };

    auto eval = [&](auto& self, BVNodeId cur) -> brass::verify::BVInterval {
        auto it = memo.find(cur);
        if (it != memo.end()) return it->second;
        brass::verify::BVInterval iv = ctx.interval(cur);
        const auto& n = ctx.nodes()[cur];
        if (n.width > 0 && n.width <= 32) {
            int64_t wLo = -(1LL << (n.width - 1));
            int64_t wHi = (1LL << (n.width - 1)) - 1;
            iv.smin = std::max<int64_t>(iv.smin, wLo);
            iv.smax = std::min<int64_t>(iv.smax, wHi);
        }
        if (n.op == BVOp::Const) {
            memo[cur] = iv;
            return iv;
        }
        if (n.op == BVOp::Var && n.width <= 32) {
            const std::string& vName = (n.imm < ctx.vars().size()) ? ctx.vars()[n.imm].name : std::string{};
            if (vName.rfind("heap:", 0) == 0 && vName.size() > 7 &&
                vName.substr(vName.size() - 7) == ".length") {
                iv.smin = 0;
                iv.smax = static_cast<int64_t>(kMaxBoundedSeqLen);
            } else {
                iv.smin = std::max<int64_t>(iv.smin, domainMin);
                iv.smax = std::min<int64_t>(iv.smax, domainMax);
            }
            memo[cur] = iv;
            return iv;
        }
        if (n.op == BVOp::SExt) {
            auto a = self(self, n.a);
            uint32_t srcW = ctx.nodes()[n.a].width;
            iv.smin = a.smin;
            iv.smax = a.smax;
            if (srcW > 0 && srcW <= 32) {
                iv.smin = std::max<int64_t>(iv.smin, -(1LL << (srcW - 1)));
                iv.smax = std::min<int64_t>(iv.smax, (1LL << (srcW - 1)) - 1);
            }
            memo[cur] = iv;
            return iv;
        }
        if (n.op == BVOp::ZExt) {
            auto a = self(self, n.a);
            uint32_t srcW = ctx.nodes()[n.a].width;
            iv.smin = (a.smin >= 0) ? a.smin : 0;
            iv.smax = (a.smin >= 0) ? a.smax : ((srcW < 63) ? ((1LL << srcW) - 1) : INT64_MAX);
            if (srcW < 63) iv.smax = std::min<int64_t>(iv.smax, (1LL << srcW) - 1);
            memo[cur] = iv;
            return iv;
        }
        if (n.width != 64) {
            memo[cur] = iv;
            return iv;
        }
        if (n.op == BVOp::Sub) {
            auto a = self(self, n.a);
            auto b = self(self, n.b);
            int64_t lo = 0, hi = 0;
            if (safeSub(a.smin, b.smax, lo) && safeSub(a.smax, b.smin, hi)) {
                iv.smin = lo;
                iv.smax = hi;
            }
        } else if (n.op == BVOp::Add) {
            auto a = self(self, n.a);
            auto b = self(self, n.b);
            int64_t lo = 0, hi = 0;
            if (safeAdd(a.smin, b.smin, lo) && safeAdd(a.smax, b.smax, hi)) {
                iv.smin = lo;
                iv.smax = hi;
            }
        } else if (n.op == BVOp::Neg) {
            auto a = self(self, n.a);
            if (a.smin > INT64_MIN) {
                iv.smin = -a.smax;
                iv.smax = -a.smin;
            }
        } else if (n.op == BVOp::Mul) {
            auto a = self(self, n.a);
            auto b = self(self, n.b);
            int64_t p1 = 0, p2 = 0, p3 = 0, p4 = 0;
            if (safeMul(a.smin, b.smin, p1) && safeMul(a.smin, b.smax, p2) &&
                safeMul(a.smax, b.smin, p3) && safeMul(a.smax, b.smax, p4)) {
                iv.smin = std::min({p1, p2, p3, p4});
                iv.smax = std::max({p1, p2, p3, p4});
            }
        } else if (n.op == BVOp::Shl && ctx.nodes()[n.b].op == BVOp::Const) {
            uint32_t sh = static_cast<uint32_t>(ctx.nodes()[n.b].imm & 63);
            if (sh < 62) {
                auto a = self(self, n.a);
                int64_t lo = 0, hi = 0;
                if (safeMul(a.smin, 1LL << sh, lo) && safeMul(a.smax, 1LL << sh, hi)) {
                    iv.smin = lo;
                    iv.smax = hi;
                }
            }
        } else if ((n.op == BVOp::LShr || n.op == BVOp::AShr) && ctx.nodes()[n.b].op == BVOp::Const) {
            uint32_t sh = static_cast<uint32_t>(ctx.nodes()[n.b].imm & 63);
            if (sh > 0 && sh < 64) {
                auto a = self(self, n.a);
                if (n.op == BVOp::AShr && a.smin > INT64_MIN && a.smax < INT64_MAX) {
                    iv.smin = a.smin >> sh;
                    iv.smax = a.smax >> sh;
                } else if (a.smin >= 0) {
                    iv.smin = a.smin >> sh;
                    iv.smax = a.smax >> sh;
                } else if (a.smin > INT64_MIN && a.smax < INT64_MAX) {
                    int64_t hi = std::max(std::abs(a.smin), std::abs(a.smax));
                    iv.smin = 0;
                    iv.smax = hi >> sh;
                }
            }
        } else if (n.op == BVOp::And) {
            auto a = self(self, n.a);
            auto b = self(self, n.b);
            if (b.smin >= 0) {
                iv.smin = 0;
                iv.smax = (a.smin >= 0) ? std::min(a.smax, b.smax) : b.smax;
            } else if (a.smin >= 0) {
                iv.smin = 0;
                iv.smax = a.smax;
            } else if (a.smin >= INT32_MIN && a.smax <= INT32_MAX && b.smin >= INT32_MIN && b.smax <= INT32_MAX) {
                iv.smin = INT32_MIN;
                iv.smax = INT32_MAX;
            }
        } else if (n.op == BVOp::Or || n.op == BVOp::Xor) {
            auto a = self(self, n.a);
            auto b = self(self, n.b);
            if (a.smin >= INT32_MIN && a.smax <= INT32_MAX && b.smin >= INT32_MIN && b.smax <= INT32_MAX) {
                iv.smin = INT32_MIN;
                iv.smax = INT32_MAX;
            }
        } else if (n.op == BVOp::SRem) {
            auto a = self(self, n.a);
            auto b = self(self, n.b);
            if (b.smin > 0 && b.smax < INT64_MAX) {
                int64_t rMax = b.smax - 1;
                iv.smin = (a.smin >= 0) ? 0 : -rMax;
                iv.smax = rMax;
            } else if (a.smin > INT64_MIN && a.smax < INT64_MAX) {
                int64_t m = std::max(std::abs(a.smin), std::abs(a.smax));
                iv.smin = -m;
                iv.smax = m;
            }
        } else if (n.op == BVOp::SDiv) {
            auto a = self(self, n.a);
            auto b = self(self, n.b);
            if (a.smin > INT64_MIN && a.smax < INT64_MAX) {
                int64_t m = std::max(std::abs(a.smin), std::abs(a.smax));
                if (b.smin > 0) {
                    iv.smin = (a.smin >= 0) ? (a.smin / b.smax) : -(m / b.smin);
                    iv.smax = m / b.smin;
                } else {
                    iv.smin = -m;
                    iv.smax = m;
                }
            }
        } else if (n.op == BVOp::Ite) {
            auto t = self(self, n.b);
            auto f = self(self, n.c);
            iv.smin = std::min(t.smin, f.smin);
            iv.smax = std::max(t.smax, f.smax);
        }
        memo[cur] = iv;
        return iv;
    };
    return eval(eval, id);
}

BVNodeId evalFloatToInt32Node(BVContext& ctx, BVNodeId f64) {
    BVNodeId sign1 = ctx.make_extract(f64, 63, 63);
    BVNodeId exp64 = ctx.make_zext(ctx.make_extract(f64, 62, 52), 53);
    BVNodeId mant64 = ctx.make_zext(ctx.make_extract(f64, 51, 0), 12);
    BVNodeId sig64 = ctx.make_binary(BVOp::Or, mant64, ctx.make_const(1ULL << 52, 64));
    BVNodeId outOfRange = ctx.make_binary(
        BVOp::Or,
        ctx.make_binary(BVOp::Ult, exp64, ctx.make_const(1023, 64), 1),
        ctx.make_binary(BVOp::Uge, exp64, ctx.make_const(1107, 64), 1), 1);
    BVNodeId isRsh = ctx.make_binary(BVOp::Ule, exp64, ctx.make_const(1075, 64), 1);
    BVNodeId rshAmt = ctx.make_binary(
        BVOp::And, ctx.make_binary(BVOp::Sub, ctx.make_const(1075, 64), exp64), ctx.make_const(63, 64));
    BVNodeId lshAmt = ctx.make_binary(
        BVOp::And, ctx.make_binary(BVOp::Sub, exp64, ctx.make_const(1075, 64)), ctx.make_const(63, 64));
    BVNodeId mag64 = ctx.make_ite(
        isRsh, ctx.make_binary(BVOp::LShr, sig64, rshAmt), ctx.make_binary(BVOp::Shl, sig64, lshAmt));
    BVNodeId signed64 = ctx.make_ite(sign1, ctx.make_unary(BVOp::Neg, mag64), mag64);
    return ctx.make_ite(outOfRange, ctx.make_const(0, 32), ctx.make_extract(signed64, 31, 0));
}

BVNodeId roundSignedInt64AsDouble(BVContext& ctx, BVNodeId val) {
    const auto& n = ctx.nodes()[val];
    if (n.op == BVOp::Const) {
        int64_t sv = static_cast<int64_t>(n.imm);
        double dv = static_cast<double>(sv);
        return ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(dv)), 64);
    }
    BVNodeId zero64 = ctx.make_const(0, 64);
    BVNodeId one64 = ctx.make_const(1, 64);
    BVNodeId isNeg = ctx.make_binary(BVOp::Slt, val, zero64, 1);
    BVNodeId mag = ctx.make_ite(isNeg, ctx.make_unary(BVOp::Neg, val), val);
    BVNodeId roundedMag = mag;
    for (uint32_t k = 1; k <= 9; ++k) {
        BVNodeId top = ctx.make_binary(BVOp::LShr, mag, ctx.make_const(52 + k, 64));
        BVNodeId inBinade = ctx.make_binary(BVOp::Eq, top, one64, 1);
        BVNodeId halfBit = ctx.make_extract(mag, k - 1, k - 1);
        BVNodeId lsbBit = ctx.make_extract(mag, k, k);
        BVNodeId stickyBit = (k > 1)
            ? ctx.make_binary(BVOp::Ne, ctx.make_extract(mag, k - 2, 0), ctx.make_const(0, k - 1), 1)
            : ctx.make_const(0, 1);
        BVNodeId roundUp1 = ctx.make_binary(BVOp::And, halfBit, ctx.make_binary(BVOp::Or, lsbBit, stickyBit, 1), 1);
        BVNodeId shifted = ctx.make_binary(BVOp::LShr, mag, ctx.make_const(k, 64));
        BVNodeId roundedShifted = ctx.make_binary(BVOp::Add, shifted, ctx.make_zext(roundUp1, 63));
        BVNodeId binadeMag = ctx.make_binary(BVOp::Shl, roundedShifted, ctx.make_const(k, 64));
        roundedMag = ctx.make_ite(inBinade, binadeMag, roundedMag);
    }
    return ctx.make_ite(isNeg, ctx.make_unary(BVOp::Neg, roundedMag), roundedMag);
}

bool evalDivOrModSymbolic(BVContext& ctx,
                          const il::Module& mod,
                          const il::Function& fn,
                          const il::Block& blk,
                          size_t instIdx,
                          BVNodeId a,
                          BVNodeId b,
                          SymbolicHeapState& heap,
                          BVNodeId& outVal,
                          std::string* outReason,
                          int32_t domainMin,
                          int32_t domainMax) {
    const auto& inst = blk.instructions[instIdx];
    const auto& aNode = ctx.nodes()[a];
    const auto& bNode = ctx.nodes()[b];

    if (bNode.op == BVOp::Const && static_cast<int64_t>(bNode.imm) == 0) {
        if (outReason) *outReason = "division by zero constant";
        return false;
    }

    auto aIv = getSignedInterval64(ctx, a, domainMin, domainMax, 16, &heap.refinedIntervals);
    auto bIv = getSignedInterval64(ctx, b, domainMin, domainMax, 16, &heap.refinedIntervals);
    static constexpr int64_t kMaxSafeInt = (1LL << 53);
    if (aIv.smin < -kMaxSafeInt || aIv.smax > kMaxSafeInt ||
        bIv.smin < -kMaxSafeInt || bIv.smax > kMaxSafeInt) {
        if (outReason) *outReason = "integer division/modulo operand can exceed 2^53 safe integer range";
        return false;
    }

    const bool bCanBeZero = (bNode.op != BVOp::Const) && (bIv.smin <= 0 && bIv.smax >= 0);
    DivUseKind useKind = classifyDivResultUsage(mod, fn, blk, instIdx);

    if (inst.op == il::Op::Mod) {
        if (bCanBeZero && useKind != DivUseKind::TruncatedToInt) {
            if (outReason) *outReason = "modulo by potentially zero divisor";
            return false;
        }
        BVNodeId rawRem = makeConstAwareDivOrRem(ctx, BVOp::SRem, a, b, domainMin, domainMax);
        if (bCanBeZero) {
            BVNodeId zero64 = ctx.make_const(0, 64);
            rawRem = ctx.make_ite(ctx.make_binary(BVOp::Eq, b, zero64, 1), zero64, rawRem);
        }
        outVal = rawRem;
        return true;
    }

    if (useKind == DivUseKind::FloorOrCeilOnly && inst.result != il::kNoValue) {
        const bool isDirectParam = (bNode.op == BVOp::Var) ||
                                   (bNode.op == BVOp::SExt && ctx.nodes()[bNode.a].op == BVOp::Var);
        if (bCanBeZero && isDirectParam) {
            if (outReason) *outReason = "division by potentially zero divisor in floor/ceil";
            return false;
        }
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
            BVNodeId sremNode = makeConstAwareDivOrRem(ctx, BVOp::SRem, a, b, domainMin, domainMax);
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

    BVNodeId rawDiv = makeConstAwareDivOrRem(ctx, BVOp::SDiv, a, b, domainMin, domainMax);
    if (bCanBeZero && useKind == DivUseKind::TruncatedToInt) {
        BVNodeId zero64 = ctx.make_const(0, 64);
        rawDiv = ctx.make_ite(ctx.make_binary(BVOp::Eq, b, zero64, 1), zero64, rawDiv);
    }
    if (aIv.smin > INT64_MIN && aIv.smax < INT64_MAX) {
        brass::verify::BVInterval divIv = ctx.interval(rawDiv);
        int64_t m = std::max(std::abs(aIv.smin), std::abs(aIv.smax));
        divIv.smin = (bIv.smin > 0 && aIv.smin >= 0) ? (aIv.smin / bIv.smax) : -m;
        divIv.smax = (bIv.smin > 0) ? (m / bIv.smin) : m;
        heap.refinedIntervals[rawDiv] = divIv;
    }
    outVal = rawDiv;
    return true;
}

BVNodeId evalFloorOrCeilSymbolic(BVContext& ctx,
                                 SymbolicHeapState& heap,
                                 il::ValueId argId,
                                 BVNodeId argVal,
                                 bool isCeil,
                                 int32_t domainMin,
                                 int32_t domainMax) {
    auto it = heap.divResults.find(argId);
    if (it == heap.divResults.end()) return argVal;

    BVNodeId a = it->second.num;
    BVNodeId b = it->second.den;
    auto aIv = getSignedInterval64(ctx, a, domainMin, domainMax, 16, &heap.refinedIntervals);
    auto bIv = getSignedInterval64(ctx, b, domainMin, domainMax, 16, &heap.refinedIntervals);
    BVNodeId zero64 = ctx.make_const(0, 64);
    BVNodeId one64 = ctx.make_const(1, 64);
    BVNodeId sdiv = makeConstAwareDivOrRem(ctx, BVOp::SDiv, a, b, domainMin, domainMax);
    BVNodeId srem = makeConstAwareDivOrRem(ctx, BVOp::SRem, a, b, domainMin, domainMax);
    if (aIv.smin > INT64_MIN && aIv.smax < INT64_MAX) {
        brass::verify::BVInterval qIv = ctx.interval(sdiv);
        int64_t m = std::max(std::abs(aIv.smin), std::abs(aIv.smax));
        qIv.smin = (bIv.smin > 0) ? ((aIv.smin >= 0) ? (aIv.smin / bIv.smax) : -(m / bIv.smin)) : -m;
        qIv.smax = (bIv.smin > 0) ? (m / bIv.smin) : m;
        heap.refinedIntervals[sdiv] = qIv;
    }

    BVNodeId remNonZero = ctx.make_binary(BVOp::Ne, srem, zero64);
    BVNodeId aNeg = ctx.make_binary(BVOp::Slt, a, zero64);
    BVNodeId bNeg = ctx.make_binary(BVOp::Slt, b, zero64);
    BVNodeId res = 0;
    if (isCeil) {
        BVNodeId sameSign = ctx.make_binary(BVOp::Eq, aNeg, bNeg);
        BVNodeId needInc = ctx.make_binary(BVOp::And, remNonZero, sameSign);
        res = ctx.make_ite(needInc, ctx.make_binary(BVOp::Add, sdiv, one64), sdiv);
    } else {
        BVNodeId diffSign = ctx.make_binary(BVOp::Ne, aNeg, bNeg);
        BVNodeId needDec = ctx.make_binary(BVOp::And, remNonZero, diffSign);
        res = ctx.make_ite(needDec, ctx.make_binary(BVOp::Sub, sdiv, one64), sdiv);
    }
    if (it->second.intOffset != 0) {
        res = ctx.make_binary(BVOp::Add, it->second.intOffset, res);
    }
    auto resIv = getSignedInterval64(ctx, res, domainMin, domainMax, 16, &heap.refinedIntervals);
    heap.refinedIntervals[res] = resIv;
    return res;
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

BVNodeId evalPowSymbolic(BVContext& ctx, BVNodeId base64, BVNodeId exp64, bool& ok,
                         int32_t domainMin, int32_t domainMax) {
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
            auto maxAbs = [](int64_t lo, int64_t hi) -> uint64_t {
                uint64_t aLo = (lo == INT64_MIN) ? (1ULL << 63) : static_cast<uint64_t>(std::abs(lo));
                uint64_t aHi = (hi == INT64_MIN) ? (1ULL << 63) : static_cast<uint64_t>(std::abs(hi));
                return std::max(aLo, aHi);
            };
            BVNodeId acc = one64;
            for (int64_t i = 0; i < expVal; ++i) {
                auto lIv = getSignedInterval64(ctx, acc, domainMin, domainMax);
                auto rIv = getSignedInterval64(ctx, base64, domainMin, domainMax);
                uint64_t maxAbsL = maxAbs(lIv.smin, lIv.smax);
                uint64_t maxAbsR = maxAbs(rIv.smin, rIv.smax);
                if (maxAbsR > 0 && maxAbsL > (1ULL << 62) / maxAbsR) {
                    ok = false;
                    return ctx.make_const(0, 64);
                }
                acc = ctx.make_binary(BVOp::Mul, acc, base64);
                if (maxAbsR > 0 && maxAbsL > (1ULL << 53) / maxAbsR) {
                    acc = roundSignedInt64AsDouble(ctx, acc);
                }
            }
            return acc;
        }
    }
    ok = false;
    return ctx.make_const(0, 64);
}

BVNodeId makeF64ConstNode(BVContext& ctx, double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(double));
    return ctx.make_const(bits, 64);
}

namespace {

bool canReachNonEmpty(const il::Function& fn, il::BlockId start, il::BlockId goal, il::BlockId minBlock) {
    if (start >= fn.blocks.size() || goal >= fn.blocks.size()) return false;
    std::queue<il::BlockId> q;
    std::vector<uint8_t> vis(fn.blocks.size(), 0);
    auto pushEdge = [&](il::BlockId dst) {
        if (dst >= fn.blocks.size() || dst < minBlock) return false;
        if (dst == goal) return true;
        if (!vis[dst]) {
            vis[dst] = 1;
            q.push(dst);
        }
        return false;
    };
    const auto& sBlk = fn.blocks[start];
    if (!sBlk.instructions.empty()) {
        const auto& term = sBlk.instructions.back();
        if (term.op == il::Op::Jump && pushEdge(term.target.block)) return true;
        if (term.op == il::Op::Branch && (pushEdge(term.target.block) || pushEdge(term.elseTarget.block))) return true;
    }
    while (!q.empty()) {
        il::BlockId cur = q.front();
        q.pop();
        const auto& blk = fn.blocks[cur];
        if (blk.instructions.empty()) continue;
        const auto& term = blk.instructions.back();
        if (term.op == il::Op::Jump && pushEdge(term.target.block)) return true;
        if (term.op == il::Op::Branch && (pushEdge(term.target.block) || pushEdge(term.elseTarget.block))) return true;
    }
    return false;
}

}  // namespace

uint32_t computeFunctionSymbolicLoopBound(const il::Function& fn, std::vector<uint8_t>& outInCycle) {
    outInCycle.assign(fn.blocks.size(), 0);
    std::vector<il::BlockId> headers;
    for (il::BlockId b = 0; b < fn.blocks.size(); ++b) {
        if (canReachNonEmpty(fn, b, b, 0)) outInCycle[b] = 1;
        if (canReachNonEmpty(fn, b, b, b)) headers.push_back(b);
    }
    bool nested = false;
    for (size_t i = 0; i < headers.size() && !nested; ++i) {
        for (size_t j = i + 1; j < headers.size(); ++j) {
            if (canReachNonEmpty(fn, headers[i], headers[j], headers[i]) &&
                canReachNonEmpty(fn, headers[j], headers[i], headers[i])) {
                nested = true;
                break;
            }
        }
    }
    if (nested && headers.size() >= 3 && fn.blocks.size() > 20) return 1u;
    if (nested) return 3u;
    if (headers.size() >= 2) return 5u;
    return 8u;
}

BVNodeId evalFloatFloorSymbolic(BVContext& ctx, BVNodeId a) {
    BVNodeId sign1 = ctx.make_extract(a, 63, 63);
    BVNodeId exp64 = ctx.make_zext(ctx.make_extract(a, 62, 52), 53);
    BVNodeId mag64 = ctx.make_binary(BVOp::And, a, ctx.make_const(0x7FFFFFFFFFFFFFFFULL, 64));
    BVNodeId isZeroOrLarge = ctx.make_binary(
        BVOp::Or, ctx.make_binary(BVOp::Eq, mag64, ctx.make_const(0, 64), 1),
        ctx.make_binary(BVOp::Uge, exp64, ctx.make_const(1075, 64), 1), 1);
    BVNodeId isSubUnit = ctx.make_binary(BVOp::Ult, exp64, ctx.make_const(1023, 64), 1);
    BVNodeId subUnitRes = ctx.make_ite(sign1, makeF64ConstNode(ctx, -1.0), ctx.make_const(0, 64));

    BVNodeId k = ctx.make_binary(BVOp::Sub, ctx.make_const(1075, 64), exp64);
    BVNodeId ulp = ctx.make_binary(BVOp::Shl, ctx.make_const(1, 64), k);
    BVNodeId fracMask = ctx.make_binary(BVOp::Sub, ulp, ctx.make_const(1, 64));
    BVNodeId truncBits = ctx.make_binary(BVOp::And, a, ctx.make_unary(BVOp::Not, fracMask));
    BVNodeId hasFrac = ctx.make_binary(BVOp::Ne, ctx.make_binary(BVOp::And, a, fracMask), ctx.make_const(0, 64), 1);
    BVNodeId midRes = ctx.make_ite(ctx.make_binary(BVOp::And, sign1, hasFrac, 1),
                                   ctx.make_binary(BVOp::Add, truncBits, ulp), truncBits);
    return ctx.make_ite(isZeroOrLarge, a, ctx.make_ite(isSubUnit, subUnitRes, midRes));
}

BVNodeId evalFloatCeilSymbolic(BVContext& ctx, BVNodeId a) {
    return ctx.make_unary(BVOp::FNeg, evalFloatFloorSymbolic(ctx, ctx.make_unary(BVOp::FNeg, a, 64)), 64);
}

BVNodeId evalFloatTruncSymbolic(BVContext& ctx, BVNodeId a) {
    return ctx.make_ite(ctx.make_extract(a, 63, 63), evalFloatCeilSymbolic(ctx, a), evalFloatFloorSymbolic(ctx, a));
}

BVNodeId evalFloatRoundSymbolic(BVContext& ctx, BVNodeId a) {
    BVNodeId sign1 = ctx.make_extract(a, 63, 63);
    BVNodeId exp64 = ctx.make_zext(ctx.make_extract(a, 62, 52), 53);
    BVNodeId mant64 = ctx.make_zext(ctx.make_extract(a, 51, 0), 12);
    BVNodeId mag64 = ctx.make_binary(BVOp::And, a, ctx.make_const(0x7FFFFFFFFFFFFFFFULL, 64));
    BVNodeId posZero = ctx.make_const(0, 64);
    BVNodeId negZero = makeF64ConstNode(ctx, -0.0);
    BVNodeId posOne = makeF64ConstNode(ctx, 1.0);
    BVNodeId negOne = makeF64ConstNode(ctx, -1.0);

    BVNodeId isZeroOrLarge = ctx.make_binary(
        BVOp::Or, ctx.make_binary(BVOp::Eq, mag64, posZero, 1),
        ctx.make_binary(BVOp::Uge, exp64, ctx.make_const(1075, 64), 1), 1);
    BVNodeId isLtHalf = ctx.make_binary(BVOp::Ult, exp64, ctx.make_const(1022, 64), 1);
    BVNodeId ltHalfRes = ctx.make_ite(sign1, negZero, posZero);
    BVNodeId isHalfToOne = ctx.make_binary(BVOp::Eq, exp64, ctx.make_const(1022, 64), 1);
    BVNodeId negHalfToOne = ctx.make_ite(ctx.make_binary(BVOp::Eq, mant64, posZero, 1), negZero, negOne);
    BVNodeId halfToOneRes = ctx.make_ite(sign1, negHalfToOne, posOne);

    BVNodeId k = ctx.make_binary(BVOp::Sub, ctx.make_const(1075, 64), exp64);
    BVNodeId ulp = ctx.make_binary(BVOp::Shl, ctx.make_const(1, 64), k);
    BVNodeId halfUlp = ctx.make_binary(BVOp::Shl, ctx.make_const(1, 64), ctx.make_binary(BVOp::Sub, k, ctx.make_const(1, 64)));
    BVNodeId fracMask = ctx.make_binary(BVOp::Sub, ulp, ctx.make_const(1, 64));
    BVNodeId fracBits = ctx.make_binary(BVOp::And, a, fracMask);
    BVNodeId truncBits = ctx.make_binary(BVOp::And, a, ctx.make_unary(BVOp::Not, fracMask));
    BVNodeId roundAway = ctx.make_ite(sign1, ctx.make_binary(BVOp::Ugt, fracBits, halfUlp, 1),
                                      ctx.make_binary(BVOp::Uge, fracBits, halfUlp, 1));
    BVNodeId midRes = ctx.make_ite(roundAway, ctx.make_binary(BVOp::Add, truncBits, ulp), truncBits);
    return ctx.make_ite(isZeroOrLarge, a,
                        ctx.make_ite(isLtHalf, ltHalfRes,
                                     ctx.make_ite(isHalfToOne, halfToOneRes, midRes)));
}

BVNodeId evalFloatPowSymbolic(BVContext& ctx, BVNodeId base64, BVNodeId exp64, bool& ok) {
    ok = true;
    const auto& eNode = ctx.nodes()[exp64];
    if (eNode.op == BVOp::Const) {
        double expD = 0.0;
        std::memcpy(&expD, &eNode.imm, sizeof(double));
        if (expD == 0.0) return makeF64ConstNode(ctx, 1.0);
        if (expD == 1.0) return ctx.make_binary(BVOp::FAdd, base64, makeF64ConstNode(ctx, -0.0), 64);
        if (expD == 2.0) return ctx.make_binary(BVOp::FMul, base64, base64, 64);
        if (expD == 3.0) {
            BVNodeId sq = ctx.make_binary(BVOp::FMul, base64, base64, 64);
            return ctx.make_binary(BVOp::FMul, sq, base64, 64);
        }
        if (expD == 4.0) {
            BVNodeId sq = ctx.make_binary(BVOp::FMul, base64, base64, 64);
            return ctx.make_binary(BVOp::FMul, sq, sq, 64);
        }
    }
    ok = false;
    return ctx.make_const(0, 64);
}

bool evalFloatBinaryOpSymbolic(BVContext& ctx,
                               il::Op op,
                               BVNodeId lhs,
                               BVNodeId rhs,
                               BVNodeId& outVal,
                               std::string* outReason) {
    using Op = il::Op;
    if (op == Op::Add) {
        outVal = ctx.make_binary(BVOp::FAdd, lhs, rhs, 64);
        return true;
    }
    if (op == Op::Sub) {
        outVal = ctx.make_binary(BVOp::FSub, lhs, rhs, 64);
        return true;
    }
    if (op == Op::Mul) {
        outVal = ctx.make_binary(BVOp::FMul, lhs, rhs, 64);
        return true;
    }
    if (op == Op::Div) {
        const auto& rNode = ctx.nodes()[rhs];
        if (rNode.op == BVOp::Const) {
            double d = 0.0;
            std::memcpy(&d, &rNode.imm, sizeof(double));
            if (std::isfinite(d) && d != 0.0) {
                uint64_t mant = rNode.imm & 0xFFFFFFFFFFFFFULL;
                uint64_t exp = (rNode.imm >> 52) & 0x7FFULL;
                if (mant != 0 || exp == 0) {
                    if (outReason) *outReason = "non-dyadic floating-point division";
                    return false;
                }
                double inv = 1.0 / d;
                uint64_t invBits = 0;
                std::memcpy(&invBits, &inv, sizeof(double));
                if (std::isfinite(inv) && (invBits & 0xFFFFFFFFFFFFFULL) == 0 && ((invBits >> 52) & 0x7FFULL) != 0) {
                    outVal = ctx.make_binary(BVOp::FMul, lhs, ctx.make_const(invBits, 64), 64);
                    return true;
                }
            }
        }
        const auto& lNode = ctx.nodes()[lhs];
        BVNodeId rawDiv = ctx.make_binary(BVOp::FDiv, lhs, rhs, 64);
        if (lNode.op == BVOp::Const) {
            double numD = 0.0;
            std::memcpy(&numD, &lNode.imm, sizeof(double));
            if (std::isfinite(numD) && numD != 0.0) {
                const double s = std::signbit(numD) ? -1.0 : 1.0;
                BVNodeId pZero = ctx.make_const(0, 64);
                BVNodeId nZero = makeF64ConstNode(ctx, -0.0);
                BVNodeId pInf = makeF64ConstNode(ctx, s * std::numeric_limits<double>::infinity());
                BVNodeId nInf = makeF64ConstNode(ctx, -s * std::numeric_limits<double>::infinity());
                BVNodeId qNaN = makeF64ConstNode(ctx, std::numeric_limits<double>::quiet_NaN());
                rawDiv = ctx.make_ite(
                    ctx.make_binary(BVOp::FNe, rhs, rhs, 1), qNaN,
                    ctx.make_ite(ctx.make_binary(BVOp::Eq, rhs, pZero, 1), pInf,
                                 ctx.make_ite(ctx.make_binary(BVOp::Eq, rhs, nZero, 1), nInf, rawDiv)));
            }
        }
        outVal = rawDiv;
        return true;
    }
    if (op == Op::Mod) {
        const auto& rNode = ctx.nodes()[rhs];
        if (rNode.op == BVOp::Const) {
            double d = 0.0;
            std::memcpy(&d, &rNode.imm, sizeof(double));
            if (std::isfinite(d) && d > 0.0 && d <= 2147483647.0 && std::trunc(d) == d) {
                int64_t modI = static_cast<int64_t>(d);
                BVNodeId lhsI64 = ctx.make_sext(evalFloatToInt32Node(ctx, lhs), 32);
                BVNodeId rhsI64 = ctx.make_const(static_cast<uint64_t>(modI), 64);
                BVNodeId remI64 = makeConstAwareDivOrRem(ctx, BVOp::SRem, lhsI64, rhsI64);
                outVal = ctx.make_unary(BVOp::SiToFp, remI64, 64);
                return true;
            }
        }
        if (outReason) *outReason = "unsupported floating-point modulo";
        return false;
    }
    return false;
}

bool tryEvalGlobalPropGetSymbolic(BVContext& ctx,
                                  const std::string& globalName,
                                  const std::string& key,
                                  bool floatMode,
                                  BVNodeId& outVal,
                                  bool& outIsMethodRef,
                                  std::string* outReason) {
    outIsMethodRef = false;
    double cVal = 0.0;
    bool isConst = false;
    if (globalName == "Number") {
        if (key == "NaN") { cVal = std::numeric_limits<double>::quiet_NaN(); isConst = true; }
        else if (key == "POSITIVE_INFINITY") { cVal = std::numeric_limits<double>::infinity(); isConst = true; }
        else if (key == "NEGATIVE_INFINITY") { cVal = -std::numeric_limits<double>::infinity(); isConst = true; }
        else if (key == "EPSILON") { cVal = std::numeric_limits<double>::epsilon(); isConst = true; }
        else if (key == "MAX_VALUE") { cVal = std::numeric_limits<double>::max(); isConst = true; }
        else if (key == "MIN_VALUE") { cVal = std::numeric_limits<double>::denorm_min(); isConst = true; }
        else if (key == "MAX_SAFE_INTEGER") { cVal = 9007199254740991.0; isConst = true; }
        else if (key == "MIN_SAFE_INTEGER") { cVal = -9007199254740991.0; isConst = true; }
    } else if (globalName == "Math") {
        if (key == "PI") { cVal = 3.14159265358979323846; isConst = true; }
        else if (key == "E") { cVal = 2.71828182845904523536; isConst = true; }
        else if (key == "LN2") { cVal = 0.693147180559945309417; isConst = true; }
        else if (key == "LN10") { cVal = 2.30258509299404568402; isConst = true; }
        else if (key == "LOG2E") { cVal = 1.44269504088896340736; isConst = true; }
        else if (key == "LOG10E") { cVal = 0.434294481903251827651; isConst = true; }
        else if (key == "SQRT2") { cVal = 1.41421356237309504880; isConst = true; }
        else if (key == "SQRT1_2") { cVal = 0.707106781186547524401; isConst = true; }
    }
    if (isConst) {
        if (!floatMode) {
            if (outReason) *outReason = "floating-point constant requires float mode";
            return false;
        }
        outVal = makeF64ConstNode(ctx, cVal);
        return true;
    }
    outIsMethodRef = true;
    return true;
}

bool functionRequiresFloatMode(const il::Module& mod, const il::Function& fn) {
    bool hasBitwiseOrElemOrMod = false;
    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.instructions) {
            using Op = il::Op;
            if (inst.op == Op::MethodCall && inst.keyIndex < mod.keyConstants.size() &&
                mod.keyConstants[inst.keyIndex] == "isSafeInteger") {
                return false;
            }
            if (inst.op == Op::BitAnd || inst.op == Op::BitOr || inst.op == Op::BitXor ||
                inst.op == Op::BitNot || inst.op == Op::Shl || inst.op == Op::Shr ||
                inst.op == Op::UShr || inst.op == Op::ToInt32 ||
                inst.op == Op::Mod || inst.op == Op::ElemGet || inst.op == Op::ElemSet ||
                inst.op == Op::Add || inst.op == Op::Sub || inst.op == Op::Mul) {
                hasBitwiseOrElemOrMod = true;
            }
        }
    }
    std::unordered_set<il::ValueId> zeroConstIds;
    std::unordered_set<il::ValueId> divTrackedIds;
    for (const auto& blk : fn.blocks) {
        for (size_t i = 0; i < blk.instructions.size(); ++i) {
            const auto& inst = blk.instructions[i];
            if (inst.op == il::Op::ConstI32 && inst.immI32 == 0 && inst.result != il::kNoValue) {
                zeroConstIds.insert(inst.result);
            } else if (inst.op == il::Op::ConstF64 && inst.immF64 == 0.0 && inst.result != il::kNoValue) {
                zeroConstIds.insert(inst.result);
            } else if (inst.op == il::Op::Div && inst.result != il::kNoValue) {
                divTrackedIds.insert(inst.result);
            } else if ((inst.op == il::Op::Box || inst.op == il::Op::Unbox || inst.op == il::Op::ToNumeric) &&
                       !inst.operands.empty() && inst.result != il::kNoValue) {
                if (zeroConstIds.count(inst.operands[0])) zeroConstIds.insert(inst.result);
                if (divTrackedIds.count(inst.operands[0])) divTrackedIds.insert(inst.result);
            }
        }
    }
    bool sawZeroMinMax = false;
    bool sawNonZeroMinMax = false;
    auto checkMinMaxZeroArg = [&](const std::vector<il::ValueId>& ops, size_t startIdx) {
        bool hasZero = false;
        for (size_t k = startIdx; k < ops.size(); ++k) {
            if (zeroConstIds.count(ops[k])) hasZero = true;
        }
        if (hasZero) sawZeroMinMax = true;
        else sawNonZeroMinMax = true;
    };
    for (const auto& blk : fn.blocks) {
        for (size_t i = 0; i < blk.instructions.size(); ++i) {
            const auto& inst = blk.instructions[i];
            using Op = il::Op;
            if (inst.op == Op::ConstF64) {
                double d = inst.immF64;
                if (std::isnan(d) || std::isinf(d) || (d == 0.0 && std::signbit(d)) || std::trunc(d) != d) {
                    return true;
                }
            } else if (inst.op == Op::Neg && !inst.operands.empty() && zeroConstIds.count(inst.operands[0])) {
                return true;
            } else if ((inst.op == Op::GlobalGet || inst.op == Op::ResolveName) &&
                       inst.keyIndex < mod.keyConstants.size()) {
                const std::string& g = mod.keyConstants[inst.keyIndex];
                if (g == "NaN" || g == "Infinity" || g == "isNaN" || g == "isFinite") return true;
            } else if (inst.op == Op::PropGet && inst.keyIndex < mod.keyConstants.size()) {
                const std::string& k = mod.keyConstants[inst.keyIndex];
                if (k == "NaN" || k == "POSITIVE_INFINITY" || k == "NEGATIVE_INFINITY" ||
                    k == "EPSILON" || k == "MAX_VALUE" || k == "MIN_VALUE" ||
                    k == "PI" || k == "E" || k == "SQRT2" || k == "SQRT1_2" || k == "LN2" || k == "LN10") {
                    return true;
                }
            } else if (inst.op == Op::MethodCall && inst.keyIndex < mod.keyConstants.size()) {
                const std::string& m = mod.keyConstants[inst.keyIndex];
                if (m == "isNaN" || m == "isFinite" || m == "is" || m == "sqrt" ||
                    m == "fround" || m == "round") {
                    return true;
                }
                if (m == "min" || m == "max") {
                    checkMinMaxZeroArg(inst.operands, 1);
                }
                if ((m == "floor" || m == "ceil" || m == "trunc") &&
                    (inst.operands.size() < 2 || !divTrackedIds.count(inst.operands[1]))) {
                    return true;
                }
            } else if (inst.op == Op::MathUnary) {
                auto kind = static_cast<il::MathUnaryFn>(inst.immI32);
                if (kind == il::MathUnaryFn::Sqrt || kind == il::MathUnaryFn::Round) {
                    return true;
                }
                if (kind == il::MathUnaryFn::Min || kind == il::MathUnaryFn::Max) {
                    checkMinMaxZeroArg(inst.operands, 0);
                }
                if ((kind == il::MathUnaryFn::Floor || kind == il::MathUnaryFn::Ceil || kind == il::MathUnaryFn::Trunc) &&
                    (inst.operands.empty() || !divTrackedIds.count(inst.operands[0]))) {
                    return true;
                }
            } else if (inst.op == Op::Div) {
                if (classifyDivResultUsage(mod, fn, blk, i) == DivUseKind::RawFloat) {
                    return true;
                }
            }
        }
    }
    if (sawZeroMinMax && !sawNonZeroMinMax && !hasBitwiseOrElemOrMod) {
        return true;
    }
    return false;
}

}  // namespace bronze::check
