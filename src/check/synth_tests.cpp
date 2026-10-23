#include "check/synth_tests.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <verify/bv_solver.hpp>

#include "check/static_checker.h"
#include "lex/lexer.h"
#include "lower/lower.h"
#include "modules/modules.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/infer.h"

namespace bronze::check {
namespace {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;
using brass::verify::SolveOutcome;

constexpr uint32_t kMaxPathDepth = 48;
constexpr size_t kMaxSynthesizedPathsPerFn = 64;

struct PathSymState {
    std::unordered_map<il::ValueId, BVNodeId> values;
    std::unordered_map<il::ValueId, int64_t> arrayLengths;
    std::unordered_map<uint64_t, BVNodeId> envSlots;
    std::unordered_map<uint32_t, std::string> globalRefs;
};

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

class FunctionTestSynthesizer {
public:
    FunctionTestSynthesizer(const il::Module& mod,
                            const il::Function& fn,
                            SynthTestsReport& report)
        : mod_(mod), fn_(fn), report_(report) {
        zero64_ = ctx_.make_const(0, 64);
        one64_ = ctx_.make_const(1, 64);
        allOnes64_ = ctx_.make_const(~uint64_t{0}, 64);
        true1_ = ctx_.make_const(1, 1);

        numParams_ = static_cast<uint32_t>(fn_.callerParamCount());
        uint32_t firstSrc = static_cast<uint32_t>(fn_.firstSourceParam());
        for (uint32_t i = 0; i < numParams_; ++i) {
            std::string pname;
            if ((firstSrc + i) < fn_.params.size()) {
                pname = fn_.params[firstSrc + i].name;
            }
            if (pname.empty()) pname = "arg_" + std::to_string(i);
            paramNames_.push_back(pname);

            BVNodeId raw32 = ctx_.make_var("arg_" + std::to_string(i), 32);
            argNodes_.push_back(ctx_.make_sext(raw32, 32));
        }
    }

    void run() {
        if (fn_.blocks.empty() || fn_.isExternal) return;

        // Count reachable non-slow-duplicate blocks from entry block 0
        std::unordered_set<uint32_t> reachable;
        std::vector<uint32_t> worklist{0};
        while (!worklist.empty()) {
            uint32_t bid = worklist.back();
            worklist.pop_back();
            if (bid >= fn_.blocks.size()) continue;
            if (fn_.blocks[bid].copyClass == il::CopyClass::Slow) continue;
            if (!reachable.insert(bid).second) continue;
            for (const auto& inst : fn_.blocks[bid].instructions) {
                if (inst.op == il::Op::Jump) {
                    worklist.push_back(inst.target.block);
                } else if (inst.op == il::Op::Branch) {
                    worklist.push_back(inst.target.block);
                    worklist.push_back(inst.elseTarget.block);
                }
            }
        }
        report_.totalBlocks += static_cast<uint32_t>(reachable.size());

        PathSymState initState;
        uint32_t firstSrc = static_cast<uint32_t>(fn_.firstSourceParam());
        for (uint32_t i = 0; i < firstSrc; ++i) {
            initState.values[i] = zero64_;
        }
        for (uint32_t i = 0; i < numParams_; ++i) {
            initState.values[firstSrc + i] = argNodes_[i];
        }

        exploreBlock(0, true1_, initState, 0, "b0");

        report_.coveredBlocks += static_cast<uint32_t>(coveredBlocks_.size());
    }

private:
    const il::Module& mod_;
    const il::Function& fn_;
    SynthTestsReport& report_;

    BVContext ctx_;
    BVNodeId zero64_ = 0;
    BVNodeId one64_ = 0;
    BVNodeId allOnes64_ = 0;
    BVNodeId true1_ = 0;

    uint32_t numParams_ = 0;
    std::vector<std::string> paramNames_;
    std::vector<BVNodeId> argNodes_;
    std::unordered_set<uint32_t> coveredBlocks_;
    size_t synthesizedCount_ = 0;

    BVNodeId getVal(const PathSymState& st, il::ValueId v) const {
        auto it = st.values.find(v);
        return (it != st.values.end()) ? it->second : zero64_;
    }

    BVNodeId toBV64(BVNodeId node) {
        uint32_t w = ctx_.width(node);
        if (w >= 64) return node;
        if (w == 1) return ctx_.make_zext(node, 64 - w);
        return ctx_.make_sext(node, 64 - w);
    }

    BVNodeId toBool1(BVNodeId node) {
        if (ctx_.width(node) == 1) return node;
        return ctx_.make_binary(BVOp::Ne, toBV64(node), zero64_);
    }

    BVNodeId makeClz32(BVNodeId x64) {
        BVNodeId x32 = ctx_.make_extract(toBV64(x64), 31, 0);
        BVNodeId res = ctx_.make_const(32, 64);
        for (uint32_t bit = 0; bit < 32; ++bit) {
            BVNodeId b = ctx_.make_extract(x32, bit, bit);
            BVNodeId lz = ctx_.make_const(31 - bit, 64);
            res = ctx_.make_ite(b, lz, res);
        }
        return res;
    }

    bool solveCondition(BVNodeId cond1,
                        std::vector<int64_t>& outArgs,
                        std::vector<uint64_t>& outModel) {
        SolveOutcome res = brass::verify::solve_nonzero(ctx_, cond1);
        if (res.status != SolveOutcome::Sat) return false;

        outModel = res.model;
        if (outModel.size() < numParams_) outModel.resize(numParams_, 0);

        static constexpr int32_t kSmallCandidates[] = {
            1, 2, 3, 4, -1, 7, 8, 10, 42, 60, 100, 400, 1900, 2000, 2023, 2024,
            2100, 2400, 0, -2, -3, -5, -10, 11, 16, -100, 255, 256, 1073741825
        };
        if (numParams_ >= 1 && numParams_ <= 2) {
            bool foundSmall = false;
            for (int32_t c0 : kSmallCandidates) {
                if (foundSmall) break;
                if (numParams_ == 1) {
                    std::vector<uint64_t> cand{static_cast<uint64_t>(static_cast<uint32_t>(c0))};
                    if (ctx_.eval_concrete(cond1, cand) != 0) {
                        outModel = std::move(cand);
                        foundSmall = true;
                    }
                } else {
                    for (int32_t c1 : kSmallCandidates) {
                        std::vector<uint64_t> cand{
                            static_cast<uint64_t>(static_cast<uint32_t>(c0)),
                            static_cast<uint64_t>(static_cast<uint32_t>(c1))
                        };
                        if (ctx_.eval_concrete(cond1, cand) != 0) {
                            outModel = std::move(cand);
                            foundSmall = true;
                            break;
                        }
                    }
                }
            }
        } else if (numParams_ > 2) {
            static constexpr int32_t kCoordCandidates[] = {
                0, 1, 2, 3, 4, -1, 5, 7, 10, 42, 60, 100, 400, 1900, 2000, 2024
            };
            for (uint32_t p = 0; p < numParams_; ++p) {
                uint64_t orig = outModel[p];
                bool simplified = false;
                for (int32_t c : kCoordCandidates) {
                    outModel[p] = static_cast<uint64_t>(static_cast<uint32_t>(c));
                    if (ctx_.eval_concrete(cond1, outModel) != 0) {
                        simplified = true;
                        break;
                    }
                }
                if (!simplified) outModel[p] = orig;
            }
        }

        outArgs.resize(numParams_, 0);
        for (uint32_t i = 0; i < numParams_; ++i) {
            int32_t s32 = static_cast<int32_t>(static_cast<uint32_t>(outModel[i]));
            outArgs[i] = static_cast<int64_t>(s32);
            outModel[i] = static_cast<uint64_t>(static_cast<uint32_t>(s32));
        }
        return true;
    }

    void tryEmitHazardReproducer(BVNodeId pathCond,
                                 BVNodeId hazardCond,
                                 uint32_t blockId,
                                 const std::string& pathTrace,
                                 const std::string& description) {
        BVNodeId fullCond = ctx_.make_binary(BVOp::And, pathCond, toBool1(hazardCond));
        std::vector<int64_t> args;
        std::vector<uint64_t> model;
        if (!solveCondition(fullCond, args, model)) return;

        SynthesizedTestCase tc;
        tc.functionName = fn_.name;
        tc.paramNames = paramNames_;
        tc.args = std::move(args);
        tc.hasExpectedReturn = false;
        tc.kind = SynthTestKind::HazardReproducer;
        tc.targetBlockId = blockId;
        tc.pathDescription = pathTrace;
        tc.hazardDescription = description;
        report_.tests.push_back(std::move(tc));
        ++report_.hazardReproducerCount;
    }

    void exploreBlock(il::BlockId blockId,
                      BVNodeId pathCond,
                      PathSymState state,
                      uint32_t depth,
                      const std::string& pathTrace) {
        if (depth > kMaxPathDepth || synthesizedCount_ >= kMaxSynthesizedPathsPerFn) return;
        if (blockId >= fn_.blocks.size()) return;

        coveredBlocks_.insert(blockId);
        const il::Block& blk = fn_.blocks[blockId];

        for (const auto& inst : blk.instructions) {
            using Op = il::Op;
            switch (inst.op) {
                case Op::ConstI32: {
                    uint64_t sext = static_cast<uint64_t>(static_cast<int64_t>(inst.immI32));
                    state.values[inst.result] = ctx_.make_const(sext, 64);
                    break;
                }
                case Op::ConstF64: {
                    int64_t iv = std::isfinite(inst.immF64) ? static_cast<int64_t>(inst.immF64) : 0;
                    state.values[inst.result] = ctx_.make_const(static_cast<uint64_t>(iv), 64);
                    break;
                }
                case Op::ConstBool:
                    state.values[inst.result] = (inst.immI32 != 0) ? one64_ : zero64_;
                    break;
                case Op::ConstNull:
                case Op::ConstUndefined:
                    state.values[inst.result] = zero64_;
                    break;
                case Op::CreateArray:
                    state.values[inst.result] = zero64_;
                    state.arrayLengths[inst.result] = std::max<int32_t>(0, inst.immI32);
                    break;
                case Op::ElemGet:
                case Op::ElemSet: {
                    if (inst.operands.size() >= 2) {
                        BVNodeId idx = toBV64(getVal(state, inst.operands[1]));
                        BVNodeId negIdx = ctx_.make_binary(BVOp::Slt, idx, zero64_);
                        auto lenIt = state.arrayLengths.find(inst.operands[0]);
                        if (lenIt != state.arrayLengths.end()) {
                            BVNodeId lenNode = ctx_.make_const(static_cast<uint64_t>(lenIt->second), 64);
                            BVNodeId geLen = ctx_.make_binary(BVOp::Sle, lenNode, idx);
                            BVNodeId oob = ctx_.make_binary(BVOp::Or, negIdx, geLen);
                            tryEmitHazardReproducer(pathCond, oob, blockId, pathTrace,
                                                    "out-of-bounds array index (index < 0 or index >= " +
                                                        std::to_string(lenIt->second) + ")");
                        } else if (ctx_.nodes()[idx].op != BVOp::Const) {
                            tryEmitHazardReproducer(pathCond, negIdx, blockId, pathTrace,
                                                    "negative array index access (index < 0)");
                        }
                    }
                    if (inst.result != il::kNoValue) state.values[inst.result] = zero64_;
                    break;
                }
                case Op::Box:
                case Op::Unbox:
                case Op::ToInt32:
                case Op::ToNumeric:
                case Op::NumTruthy:
                    state.values[inst.result] = inst.operands.empty()
                                                    ? zero64_
                                                    : toBV64(getVal(state, inst.operands[0]));
                    break;
                case Op::PinGuard:
                case Op::CensusRecord:
                case Op::KeepAlive:
                    break;
                case Op::IsNumber:
                    state.values[inst.result] = one64_;
                    break;
                case Op::IsNullish:
                    state.values[inst.result] = zero64_;
                    break;
                case Op::NumericStep: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId step = ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immI32)), 64);
                    state.values[inst.result] = ctx_.make_binary(BVOp::Add, a, step);
                    break;
                }
                case Op::MathImul: {
                    BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[0])), 31, 0);
                    BVNodeId b32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[1])), 31, 0);
                    BVNodeId prod32 = ctx_.make_binary(BVOp::Mul, a32, b32);
                    state.values[inst.result] = ctx_.make_sext(prod32, 32);
                    break;
                }
                case Op::Add:
                case Op::Sub:
                case Op::Mul:
                case Op::BitAnd:
                case Op::BitOr:
                case Op::BitXor: {
                    BVOp bop = (inst.op == Op::Add)      ? BVOp::Add
                               : (inst.op == Op::Sub)    ? BVOp::Sub
                               : (inst.op == Op::Mul)    ? BVOp::Mul
                               : (inst.op == Op::BitAnd) ? BVOp::And
                               : (inst.op == Op::BitOr)  ? BVOp::Or
                                                         : BVOp::Xor;
                    state.values[inst.result] = ctx_.make_binary(
                        bop, toBV64(getVal(state, inst.operands[0])), toBV64(getVal(state, inst.operands[1])));
                    break;
                }
                case Op::Div:
                case Op::Mod: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    if (ctx_.nodes()[b].op != BVOp::Const) {
                        BVNodeId isZeroDiv = ctx_.make_binary(BVOp::Eq, b, zero64_);
                        tryEmitHazardReproducer(pathCond, isZeroDiv, blockId, pathTrace,
                                                inst.op == Op::Div
                                                    ? "division by zero (divisor == 0 -> Infinity/NaN)"
                                                    : "modulo by zero (divisor == 0 -> NaN)");
                    }
                    BVOp bvOp = (inst.op == Op::Div) ? BVOp::SDiv : BVOp::SRem;
                    const auto& bNode = ctx_.nodes()[b];
                    if (bNode.op == BVOp::Const && static_cast<int64_t>(bNode.imm) > 0 &&
                        static_cast<int64_t>(bNode.imm) <= INT32_MAX &&
                        (bNode.imm & (bNode.imm - 1)) != 0) {
                        BVNodeId a32 = ctx_.make_extract(a, 31, 0);
                        BVNodeId b32 = ctx_.make_const(bNode.imm, 32);
                        BVNodeId r32 = brass::verify::expand_const_div_rem_bv(ctx_, bvOp, a32, b32, 32);
                        state.values[inst.result] = ctx_.make_sext(r32, 32);
                    } else {
                        state.values[inst.result] = ctx_.make_binary(bvOp, a, b);
                    }
                    break;
                }
                case Op::Neg:
                    state.values[inst.result] = ctx_.make_unary(BVOp::Neg, toBV64(getVal(state, inst.operands[0])));
                    break;
                case Op::BitNot:
                    state.values[inst.result] = ctx_.make_unary(BVOp::Not, toBV64(getVal(state, inst.operands[0])));
                    break;
                case Op::Shl: {
                    BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[0])), 31, 0);
                    BVNodeId b32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[1])), 31, 0);
                    BVNodeId sh32 = ctx_.make_binary(BVOp::And, b32, ctx_.make_const(31, 32));
                    BVNodeId r32 = ctx_.make_binary(BVOp::Shl, a32, sh32);
                    BVNodeId r64 = ctx_.make_sext(r32, 32);
                    if (ctx_.nodes()[b32].op != BVOp::Const) {
                        BVNodeId posA = ctx_.make_binary(BVOp::Slt, ctx_.make_const(0, 32), a32);
                        BVNodeId negR = ctx_.make_binary(BVOp::Slt, r64, zero64_);
                        tryEmitHazardReproducer(
                            pathCond, ctx_.make_binary(BVOp::And, posA, negR), blockId, pathTrace,
                            "signed 32-bit shift overflow into sign bit (produces negative int32)");
                    }
                    state.values[inst.result] = r64;
                    break;
                }
                case Op::Shr:
                case Op::UShr: {
                    BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[0])), 31, 0);
                    BVNodeId b32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[1])), 31, 0);
                    BVNodeId sh32 = ctx_.make_binary(BVOp::And, b32, ctx_.make_const(31, 32));
                    state.values[inst.result] = (inst.op == Op::Shr)
                                                    ? ctx_.make_sext(ctx_.make_binary(BVOp::AShr, a32, sh32), 32)
                                                    : ctx_.make_zext(ctx_.make_binary(BVOp::LShr, a32, sh32), 32);
                    break;
                }
                case Op::MathUnary: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    auto kind = static_cast<il::MathUnaryFn>(inst.immI32);
                    if (kind == il::MathUnaryFn::Abs) {
                        BVNodeId isNeg = ctx_.make_binary(BVOp::Slt, a, zero64_);
                        state.values[inst.result] = ctx_.make_ite(isNeg, ctx_.make_unary(BVOp::Neg, a), a);
                    } else if (kind == il::MathUnaryFn::Sqrt) {
                        tryEmitHazardReproducer(pathCond, ctx_.make_binary(BVOp::Slt, a, zero64_),
                                                blockId, pathTrace, "Math.sqrt domain error (arg < 0 -> NaN)");
                        state.values[inst.result] = a;
                    } else {
                        state.values[inst.result] = a;
                    }
                    break;
                }
                case Op::GlobalGet: {
                    if (inst.keyIndex < mod_.keyConstants.size()) {
                        state.globalRefs[inst.result] = mod_.keyConstants[inst.keyIndex];
                    }
                    state.values[inst.result] = zero64_;
                    break;
                }
                case Op::MethodCall: {
                    if (!inst.operands.empty() && inst.keyIndex < mod_.keyConstants.size()) {
                        auto recvIt = state.globalRefs.find(inst.operands[0]);
                        const std::string& method = mod_.keyConstants[inst.keyIndex];
                        if (recvIt != state.globalRefs.end() && recvIt->second == "Math") {
                            if (method == "abs" && inst.operands.size() >= 2) {
                                BVNodeId a = toBV64(getVal(state, inst.operands[1]));
                                BVNodeId isNeg = ctx_.make_binary(BVOp::Slt, a, zero64_);
                                state.values[inst.result] = ctx_.make_ite(isNeg, ctx_.make_unary(BVOp::Neg, a), a);
                                break;
                            }
                            if ((method == "min" || method == "max") && inst.operands.size() >= 3) {
                                BVNodeId acc = toBV64(getVal(state, inst.operands[1]));
                                for (size_t k = 2; k < inst.operands.size(); ++k) {
                                    BVNodeId rhs = toBV64(getVal(state, inst.operands[k]));
                                    BVNodeId takeRhs = (method == "min") ? ctx_.make_binary(BVOp::Slt, rhs, acc)
                                                                         : ctx_.make_binary(BVOp::Slt, acc, rhs);
                                    acc = ctx_.make_ite(takeRhs, rhs, acc);
                                }
                                state.values[inst.result] = acc;
                                break;
                            }
                            if (method == "imul" && inst.operands.size() >= 3) {
                                BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[1])), 31, 0);
                                BVNodeId b32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[2])), 31, 0);
                                state.values[inst.result] = ctx_.make_sext(ctx_.make_binary(BVOp::Mul, a32, b32), 32);
                                break;
                            }
                            if (method == "clz32" && inst.operands.size() >= 2) {
                                state.values[inst.result] = makeClz32(getVal(state, inst.operands[1]));
                                break;
                            }
                            if (method == "sign" && inst.operands.size() >= 2) {
                                BVNodeId a = toBV64(getVal(state, inst.operands[1]));
                                BVNodeId isNeg = ctx_.make_binary(BVOp::Slt, a, zero64_);
                                BVNodeId isPos = ctx_.make_binary(BVOp::Slt, zero64_, a);
                                state.values[inst.result] =
                                    ctx_.make_ite(isNeg, allOnes64_, ctx_.make_ite(isPos, one64_, zero64_));
                                break;
                            }
                            if ((method == "sqrt") && inst.operands.size() >= 2) {
                                BVNodeId a = toBV64(getVal(state, inst.operands[1]));
                                tryEmitHazardReproducer(pathCond, ctx_.make_binary(BVOp::Slt, a, zero64_),
                                                        blockId, pathTrace, "Math.sqrt domain error (arg < 0 -> NaN)");
                                state.values[inst.result] = a;
                                break;
                            }
                            if ((method == "log" || method == "log2" || method == "log10") &&
                                inst.operands.size() >= 2) {
                                BVNodeId a = toBV64(getVal(state, inst.operands[1]));
                                tryEmitHazardReproducer(
                                    pathCond, ctx_.make_binary(BVOp::Sle, a, zero64_), blockId, pathTrace,
                                    "Math." + method + " domain error (arg <= 0 -> -Infinity/NaN)");
                                state.values[inst.result] = a;
                                break;
                            }
                            if ((method == "acos" || method == "asin") && inst.operands.size() >= 2) {
                                BVNodeId a = toBV64(getVal(state, inst.operands[1]));
                                BVNodeId ltNeg1 = ctx_.make_binary(BVOp::Slt, a, allOnes64_);
                                BVNodeId gtPos1 = ctx_.make_binary(BVOp::Slt, one64_, a);
                                tryEmitHazardReproducer(
                                    pathCond, ctx_.make_binary(BVOp::Or, ltNeg1, gtPos1), blockId, pathTrace,
                                    "Math." + method + " domain error (|arg| > 1 -> NaN)");
                                state.values[inst.result] = zero64_;
                                break;
                            }
                            if (inst.operands.size() >= 2) {
                                state.values[inst.result] = toBV64(getVal(state, inst.operands[1]));
                                break;
                            }
                        }
                    }
                    if (inst.result != il::kNoValue) state.values[inst.result] = zero64_;
                    break;
                }
                case Op::CmpEq:
                case Op::StrictEq:
                case Op::LooseEq:
                case Op::CmpNe:
                case Op::CmpLt:
                case Op::RelLt:
                case Op::CmpLe:
                case Op::RelLe:
                case Op::CmpGt:
                case Op::RelGt:
                case Op::CmpGe:
                case Op::RelGe: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    bool swap = (inst.op == Op::CmpGt || inst.op == Op::RelGt ||
                                 inst.op == Op::CmpGe || inst.op == Op::RelGe);
                    BVOp cop = (inst.op == Op::CmpNe)                                    ? BVOp::Ne
                               : (inst.op == Op::CmpLt || inst.op == Op::RelLt ||
                                  inst.op == Op::CmpGt || inst.op == Op::RelGt)          ? BVOp::Slt
                               : (inst.op == Op::CmpLe || inst.op == Op::RelLe ||
                                  inst.op == Op::CmpGe || inst.op == Op::RelGe)          ? BVOp::Sle
                                                                                         : BVOp::Eq;
                    state.values[inst.result] = toBV64(ctx_.make_binary(cop, swap ? b : a, swap ? a : b));
                    break;
                }
                case Op::EnvSet:
                    if (inst.operands.size() >= 2) {
                        uint64_t key = (static_cast<uint64_t>(inst.envDepth) << 32) | inst.envIndex;
                        state.envSlots[key] = toBV64(getVal(state, inst.operands[1]));
                    }
                    break;
                case Op::EnvGet:
                case Op::EnvGetTdz: {
                    uint64_t key = (static_cast<uint64_t>(inst.envDepth) << 32) | inst.envIndex;
                    auto it = state.envSlots.find(key);
                    state.values[inst.result] = (it != state.envSlots.end()) ? it->second : zero64_;
                    break;
                }
                case Op::EnvInitTdz:
                    break;
                case Op::Jump: {
                    PathSymState nextState = state;
                    if (inst.target.block < fn_.blocks.size()) {
                        const auto& targetBlk = fn_.blocks[inst.target.block];
                        for (size_t k = 0; k < targetBlk.params.size() && k < inst.target.args.size(); ++k) {
                            nextState.values[targetBlk.params[k].id] = toBV64(getVal(state, inst.target.args[k]));
                        }
                    }
                    exploreBlock(inst.target.block, pathCond, std::move(nextState), depth + 1,
                                 pathTrace + "->b" + std::to_string(inst.target.block));
                    return;
                }
                case Op::Branch: {
                    BVNodeId condBit = toBool1(getVal(state, inst.operands[0]));
                    BVNodeId trueCond = ctx_.make_binary(BVOp::And, pathCond, condBit);
                    BVNodeId falseCond = ctx_.make_binary(BVOp::And, pathCond, ctx_.make_unary(BVOp::Not, condBit));

                    auto makeEdgeState = [&](const il::BlockTarget& edge) {
                        PathSymState edgeState = state;
                        if (edge.block < fn_.blocks.size()) {
                            const auto& targetBlk = fn_.blocks[edge.block];
                            for (size_t k = 0; k < targetBlk.params.size() && k < edge.args.size(); ++k) {
                                edgeState.values[targetBlk.params[k].id] = toBV64(getVal(state, edge.args[k]));
                            }
                        }
                        return edgeState;
                    };

                    std::vector<int64_t> dummyArgs;
                    std::vector<uint64_t> dummyModel;
                    if (solveCondition(trueCond, dummyArgs, dummyModel)) {
                        exploreBlock(inst.target.block, trueCond, makeEdgeState(inst.target), depth + 1,
                                     pathTrace + "->T(b" + std::to_string(inst.target.block) + ")");
                    }
                    if (solveCondition(falseCond, dummyArgs, dummyModel)) {
                        exploreBlock(inst.elseTarget.block, falseCond, makeEdgeState(inst.elseTarget), depth + 1,
                                     pathTrace + "->F(b" + std::to_string(inst.elseTarget.block) + ")");
                    }
                    return;
                }
                case Op::Throw: {
                    tryEmitHazardReproducer(pathCond, true1_, blockId, pathTrace,
                                            "reaches explicit throw / error exception branch");
                    return;
                }
                case Op::Ret: {
                    BVNodeId retNode = inst.operands.empty() ? zero64_ : toBV64(getVal(state, inst.operands[0]));
                    auto emitRetCase = [&](BVNodeId cond, const std::string& suffix) -> bool {
                        std::vector<int64_t> args;
                        std::vector<uint64_t> model;
                        if (!solveCondition(cond, args, model)) return false;
                        int64_t expectedRet = static_cast<int64_t>(ctx_.eval_concrete(retNode, model));
                        SynthesizedTestCase tc;
                        tc.functionName = fn_.name;
                        tc.paramNames = paramNames_;
                        tc.args = std::move(args);
                        tc.expectedReturn = expectedRet;
                        tc.hasExpectedReturn = !inst.operands.empty();
                        tc.kind = SynthTestKind::BranchCoverage;
                        tc.targetBlockId = blockId;
                        tc.pathDescription = pathTrace + suffix;
                        report_.tests.push_back(std::move(tc));
                        ++synthesizedCount_;
                        return true;
                    };

                    if (!inst.operands.empty() &&
                        ctx_.nodes()[retNode].op != BVOp::Const &&
                        ctx_.interval(retNode).umax <= 1) {
                        BVNodeId retBit = toBool1(retNode);
                        BVNodeId tCond = ctx_.make_binary(BVOp::And, pathCond, retBit);
                        BVNodeId fCond = ctx_.make_binary(BVOp::And, pathCond, ctx_.make_unary(BVOp::Not, retBit));
                        bool any = false;
                        if (emitRetCase(tCond, "[ret=1]")) any = true;
                        if (emitRetCase(fCond, "[ret=0]")) any = true;
                        if (any) return;
                    }
                    emitRetCase(pathCond, "");
                    return;
                }
                default:
                    if (inst.result != il::kNoValue) {
                        state.values[inst.result] = zero64_;
                    }
                    break;
            }
        }
    }
};

}  // namespace

const char* synthTestKindTag(SynthTestKind kind) {
    switch (kind) {
        case SynthTestKind::BranchCoverage: return "branch-coverage";
        case SynthTestKind::HazardReproducer: return "hazard-reproducer";
    }
    return "branch-coverage";
}

std::string SynthTestsReport::renderJs() const {
    std::ostringstream os;
    os << "// Auto-synthesized symbolic test suite generated by `bronze synth-tests`\n";
    if (!sourcePath.empty()) {
        os << "// Source: " << sourcePath << "\n";
    }
    os << "// Coverage: " << coveredBlocks << "/" << totalBlocks
       << " basic blocks, " << hazardReproducerCount << " hazard reproducer(s)\n\n";

    os << "function assertEq(actual, expected, label) {\n"
       << "  if (actual !== expected) {\n"
       << "    throw new Error(label + \": expected \" + expected + \", got \" + actual);\n"
       << "  }\n"
       << "}\n\n";

    for (size_t i = 0; i < tests.size(); ++i) {
        const auto& t = tests[i];
        std::ostringstream callExpr;
        callExpr << t.functionName << "(";
        for (size_t j = 0; j < t.args.size(); ++j) {
            if (j > 0) callExpr << ", ";
            callExpr << t.args[j];
        }
        callExpr << ")";

        if (t.kind == SynthTestKind::HazardReproducer) {
            os << "// [HAZARD REPRODUCER] " << t.functionName << " (" << t.pathDescription
               << "): " << t.hazardDescription << "\n";
            os << "try {\n"
               << "  " << callExpr.str() << ";\n"
               << "} catch (_err) {\n"
               << "  // Expected hazard / exception triggered\n"
               << "}\n\n";
        } else {
            os << "// [PATH " << (i + 1) << "] " << t.functionName << " (" << t.pathDescription << ")\n";
            if (t.hasExpectedReturn) {
                os << "assertEq(" << callExpr.str() << ", " << t.expectedReturn << ", \""
                   << jsonEscape(t.functionName + " " + t.pathDescription) << "\");\n\n";
            } else {
                os << callExpr.str() << ";\n\n";
            }
        }
    }
    return os.str();
}

std::string SynthTestsReport::renderJson() const {
    std::ostringstream os;
    os << "{\"sourcePath\":\"" << jsonEscape(sourcePath) << "\","
       << "\"totalBlocks\":" << totalBlocks << ","
       << "\"coveredBlocks\":" << coveredBlocks << ","
       << "\"hazardReproducerCount\":" << hazardReproducerCount << ","
       << "\"tests\":[";
    for (size_t i = 0; i < tests.size(); ++i) {
        if (i > 0) os << ",";
        const auto& t = tests[i];
        os << "{\"function\":\"" << jsonEscape(t.functionName) << "\","
           << "\"kind\":\"" << synthTestKindTag(t.kind) << "\","
           << "\"targetBlock\":" << t.targetBlockId << ","
           << "\"path\":\"" << jsonEscape(t.pathDescription) << "\","
           << "\"args\":[";
        for (size_t j = 0; j < t.args.size(); ++j) {
            if (j > 0) os << ",";
            os << t.args[j];
        }
        os << "]";
        if (t.hasExpectedReturn) {
            os << ",\"expectedReturn\":" << t.expectedReturn;
        }
        if (!t.hazardDescription.empty()) {
            os << ",\"hazard\":\"" << jsonEscape(t.hazardDescription) << "\"";
        }
        os << "}";
    }
    os << "]}\n";
    return os.str();
}

SynthTestsReport synthesizeModuleTests(const il::Module& mod,
                                       const ast::Module* astMod,
                                       const std::string& sourcePath) {
    SynthTestsReport report;
    report.sourcePath = sourcePath;

    bool hasNamedFunctions = false;
    for (const auto& fn : mod.functions) {
        if (fn.isExternal || fn.isEntryPoint || fn.name == "main") continue;
        hasNamedFunctions = true;
        FunctionTestSynthesizer synth(mod, fn, report);
        synth.run();
    }

    if (!hasNamedFunctions) {
        for (const auto& fn : mod.functions) {
            if (!fn.isExternal && (fn.isEntryPoint || fn.name == "main")) {
                FunctionTestSynthesizer synth(mod, fn, report);
                synth.run();
            }
        }
    }

    if (astMod != nullptr) {
        CheckReport staticRep = checkModule(*astMod, nullptr, &mod);
        for (const auto& finding : staticRep.findings) {
            SynthesizedTestCase tc;
            tc.functionName = "<static-check>";
            tc.kind = SynthTestKind::HazardReproducer;
            tc.pathDescription = bugKindTag(finding.kind);
            tc.hazardDescription = finding.message;
            report.tests.push_back(std::move(tc));
            ++report.hazardReproducerCount;
        }
    }

    return report;
}

SynthTestsReport synthesizeSourceTests(std::string_view src, std::string* errorOut) {
    SourceSet sources;
    const auto& buf = sources.add("input.js", std::string(src));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) {
        if (errorOut) *errorOut = diags.render(buf);
        return {};
    }
    auto astMod = Parser(std::move(tokens), diags).parseModule("input.js");
    if (diags.hasErrors() || !astMod) {
        if (errorOut) *errorOut = diags.render(buf);
        return {};
    }
    auto inferred = types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) {
        if (errorOut) *errorOut = diags.render(buf);
        return {};
    }
    auto ilMod = lower::lowerModule(*astMod, diags, &*inferred, nullptr, &sources);
    if (diags.hasErrors() || !ilMod) {
        if (errorOut) *errorOut = diags.render(buf);
        return {};
    }
    return synthesizeModuleTests(*ilMod, astMod.get(), "input.js");
}

int runSynthTests(const std::string& inputPath,
                  const std::string& outputPath,
                  bool jsonOutput,
                  std::ostream& out,
                  std::ostream& err,
                  const std::vector<modules::ModuleRoot>& moduleRoots,
                  const std::string& importMapPath) {
    SourceSet sources;
    DiagnosticSink diags;
    auto astMod = modules::loadProgram(inputPath, sources, diags, {moduleRoots, importMapPath});
    if (diags.hasErrors() || !astMod) {
        err << diags.render(sources);
        return 1;
    }

    auto inferred = types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) {
        err << diags.render(sources);
        return 1;
    }

    auto ilMod = lower::lowerModule(*astMod, diags, &*inferred, nullptr, &sources);
    if (diags.hasErrors() || !ilMod) {
        err << diags.render(sources);
        return 1;
    }

    SynthTestsReport report = synthesizeModuleTests(*ilMod, astMod.get(), inputPath);
    std::string rendered = jsonOutput ? report.renderJson() : report.renderJs();

    if (!outputPath.empty()) {
        std::ofstream ofs(outputPath, std::ios::binary);
        if (!ofs) {
            err << "error: cannot write synthesized tests to " << outputPath << "\n";
            return 1;
        }
        ofs << rendered;
        return 0;
    }

    out << rendered;
    return 0;
}

}  // namespace bronze::check
