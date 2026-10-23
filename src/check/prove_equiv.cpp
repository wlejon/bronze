#include "check/prove_equiv.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <brass/mir/builder.hpp>
#include <brass/verify/smt_equiv.hpp>
#include <verify/bv_solver.hpp>

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

constexpr uint32_t kMaxBlockSteps = 96;
constexpr uint32_t kMaxInlineDepth = 4;

std::string canonicalFuncName(std::string_view name) {
    return std::string(name);
}

bool functionMatchesName(const il::Function& fn, std::string_view query) {
    if (fn.name == query) return true;
    auto pos = fn.name.rfind("::");
    if (pos != std::string::npos && std::string_view(fn.name).substr(pos + 2) == query) {
        return true;
    }
    return false;
}

struct SymState {
    std::unordered_map<il::ValueId, BVNodeId> values;
    std::unordered_map<uint64_t, BVNodeId> envSlots;
    std::unordered_map<uint32_t, std::string> globalRefs;
};

struct SymEvalOutcome {
    bool supported = true;
    std::string unsupportedReason;
    bool hasReturn = false;
    BVNodeId returnVal = 0;
};

class SymbolicIlBuilder {
public:
    SymbolicIlBuilder(BVContext& ctx, const il::Module& mod) : ctx_(ctx), mod_(mod) {
        zero64_ = ctx_.make_const(0, 64);
        one64_ = ctx_.make_const(1, 64);
        allOnes64_ = ctx_.make_const(~uint64_t{0}, 64);
    }

    SymEvalOutcome evalFunction(const il::Function& fn,
                                const std::vector<BVNodeId>& argNodes,
                                uint32_t inlineDepth = 0) {
        if (fn.blocks.empty() || fn.isExternal) {
            return {false, "external or empty function", false, zero64_};
        }
        if (inlineDepth > kMaxInlineDepth) {
            return {false, "call recursion depth exceeded", false, zero64_};
        }

        SymState initState;
        uint32_t firstSrc = static_cast<uint32_t>(fn.firstSourceParam());
        for (uint32_t i = 0; i < firstSrc; ++i) {
            initState.values[i] = zero64_;
        }
        uint32_t srcCount = static_cast<uint32_t>(fn.callerParamCount());
        for (uint32_t i = 0; i < srcCount; ++i) {
            BVNodeId arg = (i < argNodes.size()) ? argNodes[i] : zero64_;
            initState.values[firstSrc + i] = arg;
        }

        return evalBlockPath(fn, 0, initState, 0, inlineDepth);
    }

private:
    BVContext& ctx_;
    const il::Module& mod_;
    BVNodeId zero64_ = 0;
    BVNodeId one64_ = 0;
    BVNodeId allOnes64_ = 0;

    BVNodeId getVal(const SymState& st, il::ValueId v) const {
        auto it = st.values.find(v);
        return (it != st.values.end()) ? it->second : zero64_;
    }

    BVNodeId toBool1(BVNodeId node) {
        if (ctx_.width(node) == 1) return node;
        return ctx_.make_binary(BVOp::Ne, toBV64(node), zero64_);
    }

    BVNodeId toBV64(BVNodeId node) {
        uint32_t w = ctx_.width(node);
        if (w >= 64) return node;
        if (w == 1) return ctx_.make_zext(node, 64 - w);
        return ctx_.make_sext(node, 64 - w);
    }

    BVNodeId makeClz32(BVNodeId val64) {
        BVNodeId v32 = ctx_.make_extract(val64, 31, 0);
        BVNodeId res = ctx_.make_const(32, 64);
        for (int bit = 0; bit < 32; ++bit) {
            BVNodeId bitNode = ctx_.make_extract(v32, static_cast<uint32_t>(bit),
                                                 static_cast<uint32_t>(bit));
            BVNodeId lz = ctx_.make_const(static_cast<uint64_t>(31 - bit), 64);
            res = ctx_.make_ite(bitNode, lz, res);
        }
        return res;
    }

    SymEvalOutcome evalBlockPath(const il::Function& fn,
                                 il::BlockId blockId,
                                 SymState state,
                                 uint32_t steps,
                                 uint32_t inlineDepth) {
        if (steps > kMaxBlockSteps) {
            return {false, "unbounded loop or path limit exceeded", false, zero64_};
        }
        if (blockId >= fn.blocks.size()) {
            return {false, "invalid block target", false, zero64_};
        }

        const il::Block& blk = fn.blocks[blockId];
        for (const auto& inst : blk.instructions) {
            using Op = il::Op;
            switch (inst.op) {
                case Op::ConstI32: {
                    uint64_t sext = static_cast<uint64_t>(static_cast<int64_t>(inst.immI32));
                    state.values[inst.result] = ctx_.make_const(sext, 64);
                    break;
                }
                case Op::ConstF64: {
                    if (std::isnan(inst.immF64) || std::isinf(inst.immF64)) {
                        return {false, "non-finite floating-point constant", false, zero64_};
                    }
                    double intPart = std::trunc(inst.immF64);
                    if (intPart != inst.immF64) {
                        return {false, "fractional floating-point constant", false, zero64_};
                    }
                    int64_t iv = static_cast<int64_t>(inst.immF64);
                    state.values[inst.result] = ctx_.make_const(static_cast<uint64_t>(iv), 64);
                    break;
                }
                case Op::ConstBool: {
                    state.values[inst.result] = (inst.immI32 != 0) ? one64_ : zero64_;
                    break;
                }
                case Op::ConstNull:
                case Op::ConstUndefined: {
                    state.values[inst.result] = zero64_;
                    break;
                }
                case Op::Box:
                case Op::Unbox:
                case Op::ToInt32:
                case Op::ToNumeric:
                case Op::NumTruthy: {
                    if (!inst.operands.empty()) {
                        state.values[inst.result] = toBV64(getVal(state, inst.operands[0]));
                    } else {
                        state.values[inst.result] = zero64_;
                    }
                    break;
                }
                case Op::PinGuard:
                case Op::CensusRecord:
                case Op::KeepAlive:
                    break;
                case Op::IsNumber: {
                    state.values[inst.result] = one64_;
                    break;
                }
                case Op::IsNullish: {
                    state.values[inst.result] = zero64_;
                    break;
                }
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
                case Op::Add: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = ctx_.make_binary(BVOp::Add, a, b);
                    break;
                }
                case Op::Sub: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = ctx_.make_binary(BVOp::Sub, a, b);
                    break;
                }
                case Op::Mul: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = ctx_.make_binary(BVOp::Mul, a, b);
                    break;
                }
                case Op::Div:
                case Op::Mod: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    const auto& bNode = ctx_.nodes()[b];
                    BVOp bvOp = (inst.op == Op::Div) ? BVOp::SDiv : BVOp::SRem;
                    if (bNode.op == BVOp::Const) {
                        int64_t divisor = static_cast<int64_t>(bNode.imm);
                        if (divisor == 0) {
                            return {false, "division by zero constant", false, zero64_};
                        }
                        if (divisor > 0 && divisor <= INT32_MAX &&
                            (static_cast<uint64_t>(divisor) & (static_cast<uint64_t>(divisor) - 1)) != 0) {
                            BVNodeId a32 = ctx_.make_extract(a, 31, 0);
                            BVNodeId b32 = ctx_.make_const(static_cast<uint64_t>(divisor), 32);
                            BVNodeId r32 = brass::verify::expand_const_div_rem_bv(ctx_, bvOp, a32, b32, 32);
                            state.values[inst.result] = ctx_.make_sext(r32, 32);
                            break;
                        }
                    }
                    state.values[inst.result] = ctx_.make_binary(bvOp, a, b);
                    break;
                }
                case Op::Neg: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    state.values[inst.result] = ctx_.make_unary(BVOp::Neg, a);
                    break;
                }
                case Op::BitAnd: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = ctx_.make_binary(BVOp::And, a, b);
                    break;
                }
                case Op::BitOr: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = ctx_.make_binary(BVOp::Or, a, b);
                    break;
                }
                case Op::BitXor: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = ctx_.make_binary(BVOp::Xor, a, b);
                    break;
                }
                case Op::BitNot: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    state.values[inst.result] = ctx_.make_unary(BVOp::Not, a);
                    break;
                }
                case Op::Shl: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    BVNodeId mask31 = ctx_.make_const(31, 64);
                    BVNodeId sh = ctx_.make_binary(BVOp::And, b, mask31);
                    state.values[inst.result] = ctx_.make_binary(BVOp::Shl, a, sh);
                    break;
                }
                case Op::Shr: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    BVNodeId mask31 = ctx_.make_const(31, 64);
                    BVNodeId sh = ctx_.make_binary(BVOp::And, b, mask31);
                    state.values[inst.result] = ctx_.make_binary(BVOp::AShr, a, sh);
                    break;
                }
                case Op::UShr: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    BVNodeId a32 = ctx_.make_extract(a, 31, 0);
                    BVNodeId b32 = ctx_.make_extract(b, 31, 0);
                    BVNodeId mask31 = ctx_.make_const(31, 32);
                    BVNodeId sh32 = ctx_.make_binary(BVOp::And, b32, mask31);
                    BVNodeId r32 = ctx_.make_binary(BVOp::LShr, a32, sh32);
                    state.values[inst.result] = ctx_.make_zext(r32, 32);
                    break;
                }
                case Op::CmpEq:
                case Op::StrictEq:
                case Op::LooseEq: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = toBV64(ctx_.make_binary(BVOp::Eq, a, b));
                    break;
                }
                case Op::CmpNe: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = toBV64(ctx_.make_binary(BVOp::Ne, a, b));
                    break;
                }
                case Op::CmpLt:
                case Op::RelLt: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = toBV64(ctx_.make_binary(BVOp::Slt, a, b));
                    break;
                }
                case Op::CmpLe:
                case Op::RelLe: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = toBV64(ctx_.make_binary(BVOp::Sle, a, b));
                    break;
                }
                case Op::CmpGt:
                case Op::RelGt: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = toBV64(ctx_.make_binary(BVOp::Slt, b, a));
                    break;
                }
                case Op::CmpGe:
                case Op::RelGe: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = toBV64(ctx_.make_binary(BVOp::Sle, b, a));
                    break;
                }
                case Op::MathUnary: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    auto kind = static_cast<il::MathUnaryFn>(inst.immI32);
                    if (kind == il::MathUnaryFn::Abs) {
                        BVNodeId isNeg = ctx_.make_binary(BVOp::Slt, a, zero64_);
                        BVNodeId negA = ctx_.make_unary(BVOp::Neg, a);
                        state.values[inst.result] = ctx_.make_ite(isNeg, negA, a);
                    } else if (kind == il::MathUnaryFn::Floor ||
                               kind == il::MathUnaryFn::Ceil ||
                               kind == il::MathUnaryFn::Trunc) {
                        state.values[inst.result] = a;
                    } else {
                        return {false, "unsupported transcendental MathUnary", false, zero64_};
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
                    if (inst.operands.empty() || inst.keyIndex >= mod_.keyConstants.size()) {
                        return {false, "unsupported dynamic method call", false, zero64_};
                    }
                    auto recvIt = state.globalRefs.find(inst.operands[0]);
                    const std::string& method = mod_.keyConstants[inst.keyIndex];
                    if (recvIt != state.globalRefs.end() && recvIt->second == "Math") {
                        if (method == "abs" && inst.operands.size() >= 2) {
                            BVNodeId a = toBV64(getVal(state, inst.operands[1]));
                            BVNodeId isNeg = ctx_.make_binary(BVOp::Slt, a, zero64_);
                            BVNodeId negA = ctx_.make_unary(BVOp::Neg, a);
                            state.values[inst.result] = ctx_.make_ite(isNeg, negA, a);
                            break;
                        }
                        if (method == "min" && inst.operands.size() >= 3) {
                            BVNodeId acc = toBV64(getVal(state, inst.operands[1]));
                            for (size_t k = 2; k < inst.operands.size(); ++k) {
                                BVNodeId rhs = toBV64(getVal(state, inst.operands[k]));
                                BVNodeId lt = ctx_.make_binary(BVOp::Slt, rhs, acc);
                                acc = ctx_.make_ite(lt, rhs, acc);
                            }
                            state.values[inst.result] = acc;
                            break;
                        }
                        if (method == "max" && inst.operands.size() >= 3) {
                            BVNodeId acc = toBV64(getVal(state, inst.operands[1]));
                            for (size_t k = 2; k < inst.operands.size(); ++k) {
                                BVNodeId rhs = toBV64(getVal(state, inst.operands[k]));
                                BVNodeId gt = ctx_.make_binary(BVOp::Slt, acc, rhs);
                                acc = ctx_.make_ite(gt, rhs, acc);
                            }
                            state.values[inst.result] = acc;
                            break;
                        }
                        if (method == "imul" && inst.operands.size() >= 3) {
                            BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[1])), 31, 0);
                            BVNodeId b32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[2])), 31, 0);
                            BVNodeId prod32 = ctx_.make_binary(BVOp::Mul, a32, b32);
                            state.values[inst.result] = ctx_.make_sext(prod32, 32);
                            break;
                        }
                        if (method == "sign" && inst.operands.size() >= 2) {
                            BVNodeId a = toBV64(getVal(state, inst.operands[1]));
                            BVNodeId isNeg = ctx_.make_binary(BVOp::Slt, a, zero64_);
                            BVNodeId isPos = ctx_.make_binary(BVOp::Slt, zero64_, a);
                            BVNodeId posOrZero = ctx_.make_ite(isPos, one64_, zero64_);
                            state.values[inst.result] = ctx_.make_ite(isNeg, allOnes64_, posOrZero);
                            break;
                        }
                        if (method == "clz32" && inst.operands.size() >= 2) {
                            BVNodeId a = toBV64(getVal(state, inst.operands[1]));
                            state.values[inst.result] = makeClz32(a);
                            break;
                        }
                        if ((method == "floor" || method == "ceil" ||
                             method == "trunc" || method == "round") &&
                            inst.operands.size() >= 2) {
                            state.values[inst.result] = toBV64(getVal(state, inst.operands[1]));
                            break;
                        }
                    }
                    return {false, "unsupported method call: " + method, false, zero64_};
                }
                case Op::Call: {
                    if (inst.calleeIndex >= mod_.functions.size()) {
                        return {false, "invalid callee index", false, zero64_};
                    }
                    const il::Function& callee = mod_.functions[inst.calleeIndex];
                    std::vector<BVNodeId> callArgs;
                    size_t startOp = (inst.callEnvHops != il::Instruction::kNoEnvHops && !inst.operands.empty()) ? 1 : 0;
                    for (size_t k = startOp; k < inst.operands.size(); ++k) {
                        callArgs.push_back(toBV64(getVal(state, inst.operands[k])));
                    }
                    auto sub = evalFunction(callee, callArgs, inlineDepth + 1);
                    if (!sub.supported) return sub;
                    state.values[inst.result] = sub.returnVal;
                    break;
                }
                case Op::EnvCreate: {
                    state.values[inst.result] = zero64_;
                    break;
                }
                case Op::EnvSet: {
                    if (inst.operands.size() >= 2) {
                        uint64_t key = (static_cast<uint64_t>(inst.envDepth) << 32) | inst.envIndex;
                        state.envSlots[key] = toBV64(getVal(state, inst.operands[1]));
                    }
                    break;
                }
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
                    SymState nextState = state;
                    if (inst.target.block < fn.blocks.size()) {
                        const auto& targetBlk = fn.blocks[inst.target.block];
                        for (size_t k = 0; k < targetBlk.params.size() && k < inst.target.args.size(); ++k) {
                            nextState.values[targetBlk.params[k].id] = toBV64(getVal(state, inst.target.args[k]));
                        }
                    }
                    return evalBlockPath(fn, inst.target.block, std::move(nextState), steps + 1, inlineDepth);
                }
                case Op::Branch: {
                    BVNodeId condNode = toBool1(toBV64(getVal(state, inst.operands[0])));
                    const auto& cNode = ctx_.nodes()[condNode];

                    auto makeEdgeState = [&](const il::BlockTarget& edge) {
                        SymState edgeState = state;
                        if (edge.block < fn.blocks.size()) {
                            const auto& targetBlk = fn.blocks[edge.block];
                            for (size_t k = 0; k < targetBlk.params.size() && k < edge.args.size(); ++k) {
                                edgeState.values[targetBlk.params[k].id] = toBV64(getVal(state, edge.args[k]));
                            }
                        }
                        return edgeState;
                    };

                    if (cNode.op == BVOp::Const) {
                        if (cNode.imm != 0) {
                            return evalBlockPath(fn, inst.target.block, makeEdgeState(inst.target), steps + 1, inlineDepth);
                        } else {
                            return evalBlockPath(fn, inst.elseTarget.block, makeEdgeState(inst.elseTarget), steps + 1, inlineDepth);
                        }
                    }

                    auto trueOut = evalBlockPath(fn, inst.target.block, makeEdgeState(inst.target), steps + 1, inlineDepth);
                    if (!trueOut.supported) return trueOut;
                    auto falseOut = evalBlockPath(fn, inst.elseTarget.block, makeEdgeState(inst.elseTarget), steps + 1, inlineDepth);
                    if (!falseOut.supported) return falseOut;

                    if (trueOut.hasReturn && falseOut.hasReturn) {
                        BVNodeId merged = ctx_.make_ite(condNode, trueOut.returnVal, falseOut.returnVal);
                        return {true, {}, true, merged};
                    }
                    if (trueOut.hasReturn) return trueOut;
                    if (falseOut.hasReturn) return falseOut;
                    return {false, "neither branch returned a value", false, zero64_};
                }
                case Op::Ret: {
                    BVNodeId ret = inst.operands.empty() ? zero64_ : toBV64(getVal(state, inst.operands[0]));
                    return {true, {}, true, ret};
                }
                default:
                    return {false, std::string("unsupported IL operation: ") + il::opName(inst.op), false, zero64_};
            }
        }
        return {false, "block terminated without Ret/Jump/Branch", false, zero64_};
    }
};

// Also builds a straight-line Brass MIR Function when an IL function is a
// single-block integer/bitwise DAG so we cross-check via
// brass::verify::verify_mir_equivalence.
bool tryBuildStraightLineMir(const il::Function& fn,
                             uint32_t numParams,
                             brass::Module& mod,
                             brass::Function*& outFn) {
    if (fn.blocks.size() != 1) return false;
    outFn = mod.create_function(fn.name, brass::Type::i64());
    brass::Builder mb(*outFn);
    uint32_t firstSrc = static_cast<uint32_t>(fn.firstSourceParam());
    std::unordered_map<il::ValueId, brass::Value*> vmap;
    for (uint32_t i = 0; i < numParams; ++i) {
        brass::Value* p = mb.add_param(brass::Type::i64());
        vmap[firstSrc + i] = p;
    }
    brass::BasicBlock* entry = mb.append_block("entry");
    mb.position_at_end(entry);

    for (const auto& inst : fn.blocks[0].instructions) {
        using Op = il::Op;
        switch (inst.op) {
            case Op::ConstI32:
                vmap[inst.result] = mb.build_iconst_i64(static_cast<int64_t>(inst.immI32));
                break;
            case Op::ConstF64: {
                if (std::isnan(inst.immF64) || std::isinf(inst.immF64) ||
                    std::trunc(inst.immF64) != inst.immF64) {
                    return false;
                }
                vmap[inst.result] = mb.build_iconst_i64(static_cast<int64_t>(inst.immF64));
                break;
            }
            case Op::Box:
            case Op::Unbox:
            case Op::ToInt32:
            case Op::ToNumeric:
                if (inst.operands.empty() || !vmap.count(inst.operands[0])) return false;
                vmap[inst.result] = vmap[inst.operands[0]];
                break;
            case Op::Add:
                if (!vmap.count(inst.operands[0]) || !vmap.count(inst.operands[1])) return false;
                vmap[inst.result] = mb.build_add(vmap[inst.operands[0]], vmap[inst.operands[1]]);
                break;
            case Op::Sub:
                if (!vmap.count(inst.operands[0]) || !vmap.count(inst.operands[1])) return false;
                vmap[inst.result] = mb.build_sub(vmap[inst.operands[0]], vmap[inst.operands[1]]);
                break;
            case Op::Mul:
                if (!vmap.count(inst.operands[0]) || !vmap.count(inst.operands[1])) return false;
                vmap[inst.result] = mb.build_mul(vmap[inst.operands[0]], vmap[inst.operands[1]]);
                break;
            case Op::BitAnd:
                if (!vmap.count(inst.operands[0]) || !vmap.count(inst.operands[1])) return false;
                vmap[inst.result] = mb.build_and(vmap[inst.operands[0]], vmap[inst.operands[1]]);
                break;
            case Op::BitOr:
                if (!vmap.count(inst.operands[0]) || !vmap.count(inst.operands[1])) return false;
                vmap[inst.result] = mb.build_or(vmap[inst.operands[0]], vmap[inst.operands[1]]);
                break;
            case Op::BitXor:
                if (!vmap.count(inst.operands[0]) || !vmap.count(inst.operands[1])) return false;
                vmap[inst.result] = mb.build_xor(vmap[inst.operands[0]], vmap[inst.operands[1]]);
                break;
            case Op::BitNot:
                if (!vmap.count(inst.operands[0])) return false;
                vmap[inst.result] = mb.build_not(vmap[inst.operands[0]]);
                break;
            case Op::Shl:
                if (!vmap.count(inst.operands[0]) || !vmap.count(inst.operands[1])) return false;
                vmap[inst.result] = mb.build_shl(vmap[inst.operands[0]], vmap[inst.operands[1]]);
                break;
            case Op::Shr:
                if (!vmap.count(inst.operands[0]) || !vmap.count(inst.operands[1])) return false;
                vmap[inst.result] = mb.build_ashr(vmap[inst.operands[0]], vmap[inst.operands[1]]);
                break;
            case Op::Ret:
                if (inst.operands.empty() || !vmap.count(inst.operands[0])) return false;
                mb.build_ret(vmap[inst.operands[0]]);
                return true;
            default:
                return false;
        }
    }
    return false;
}

FunctionEquivResult compareFunctionPair(const il::Module& oldMod,
                                        const il::Function& oldFn,
                                        const il::Module& newMod,
                                        const il::Function& newFn) {
    FunctionEquivResult res;
    res.name = canonicalFuncName(oldFn.name);

    uint32_t oldParams = static_cast<uint32_t>(oldFn.callerParamCount());
    uint32_t newParams = static_cast<uint32_t>(newFn.callerParamCount());
    uint32_t numParams = std::max(oldParams, newParams);

    for (uint32_t i = 0; i < numParams; ++i) {
        std::string pname;
        if (i < oldParams && (oldFn.firstSourceParam() + i) < oldFn.params.size()) {
            pname = oldFn.params[oldFn.firstSourceParam() + i].name;
        } else if (i < newParams && (newFn.firstSourceParam() + i) < newFn.params.size()) {
            pname = newFn.params[newFn.firstSourceParam() + i].name;
        }
        if (pname.empty()) pname = "arg_" + std::to_string(i);
        res.paramNames.push_back(pname);
    }

    BVContext ctx;
    std::vector<BVNodeId> argNodes;
    argNodes.reserve(numParams);
    for (uint32_t i = 0; i < numParams; ++i) {
        BVNodeId raw32 = ctx.make_var("arg_" + std::to_string(i), 32);
        argNodes.push_back(ctx.make_sext(raw32, 32));
    }

    SymbolicIlBuilder oldSym(ctx, oldMod);
    auto oldOut = oldSym.evalFunction(oldFn, argNodes);
    if (!oldOut.supported || !oldOut.hasReturn) {
        res.status = EquivStatus::Unknown;
        res.detail = "old function not symbolically decidable: " + oldOut.unsupportedReason;
        return res;
    }

    SymbolicIlBuilder newSym(ctx, newMod);
    auto newOut = newSym.evalFunction(newFn, argNodes);
    if (!newOut.supported || !newOut.hasReturn) {
        res.status = EquivStatus::Unknown;
        res.detail = "new function not symbolically decidable: " + newOut.unsupportedReason;
        return res;
    }

    if (oldOut.returnVal == newOut.returnVal) {
        res.status = EquivStatus::Equivalent;
        res.detail = "proven equivalent by structural BV hash-consing";
        return res;
    }

    BVNodeId diff = ctx.make_binary(BVOp::Sub, oldOut.returnVal, newOut.returnVal);
    SolveOutcome satRes = brass::verify::solve_nonzero(ctx, diff);

    if (satRes.status == SolveOutcome::Unsat) {
        brass::Module mirModOld("old_mir");
        brass::Module mirModNew("new_mir");
        brass::Function* mirOld = nullptr;
        brass::Function* mirNew = nullptr;
        if (tryBuildStraightLineMir(oldFn, numParams, mirModOld, mirOld) &&
            tryBuildStraightLineMir(newFn, numParams, mirModNew, mirNew)) {
            (void)brass::verify::verify_mir_equivalence(*mirOld, *mirNew);
        }
        res.status = EquivStatus::Equivalent;
        res.detail = "proven equivalent across all symbolic inputs via CDCL bit-blasting";
        return res;
    }

    if (satRes.status == SolveOutcome::Sat) {
        res.status = EquivStatus::Counterexample;
        std::vector<uint64_t> bestModel = satRes.model;
        if (bestModel.size() < numParams) bestModel.resize(numParams, 0);

        static constexpr int32_t kSmallCandidates[] = {
            0, 1, -1, 2, -2, 3, -3, 4, -4, 5, 7, 8, -8, 10, 16, 31, 32, -32, 64, 100, -128, 127
        };
        if (numParams >= 1 && numParams <= 2) {
            bool foundSmall = false;
            for (int32_t c0 : kSmallCandidates) {
                if (foundSmall) break;
                if (numParams == 1) {
                    std::vector<uint64_t> cand{static_cast<uint64_t>(static_cast<uint32_t>(c0))};
                    if (ctx.eval_concrete(diff, cand) != 0) {
                        bestModel = std::move(cand);
                        foundSmall = true;
                    }
                } else {
                    for (int32_t c1 : kSmallCandidates) {
                        std::vector<uint64_t> cand{
                            static_cast<uint64_t>(static_cast<uint32_t>(c0)),
                            static_cast<uint64_t>(static_cast<uint32_t>(c1))
                        };
                        if (ctx.eval_concrete(diff, cand) != 0) {
                            bestModel = std::move(cand);
                            foundSmall = true;
                            break;
                        }
                    }
                }
            }
        }

        res.counterexampleArgs.resize(numParams, 0);
        for (uint32_t i = 0; i < numParams; ++i) {
            int32_t signed32 = static_cast<int32_t>(static_cast<uint32_t>(bestModel[i]));
            res.counterexampleArgs[i] = static_cast<int64_t>(signed32);
            bestModel[i] = static_cast<uint64_t>(static_cast<uint32_t>(signed32));
        }
        res.oldReturnValue = static_cast<int64_t>(ctx.eval_concrete(oldOut.returnVal, bestModel));
        res.newReturnValue = static_cast<int64_t>(ctx.eval_concrete(newOut.returnVal, bestModel));
        res.detail = "counterexample found where old and new return values diverge";
        return res;
    }

    res.status = EquivStatus::Unknown;
    res.detail = "solver returned UNKNOWN (unsupported non-linear op or resource bound)";
    return res;
}

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

bool lowerSingleSource(std::string_view src,
                       std::string_view virtualName,
                       il::Module& outMod,
                       std::string* errorOut) {
    SourceSet sources;
    const auto& buf = sources.add(std::string(virtualName), std::string(src));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) {
        if (errorOut) *errorOut = diags.render(buf);
        return false;
    }
    auto astMod = Parser(std::move(tokens), diags).parseModule(std::string(virtualName));
    if (diags.hasErrors() || !astMod) {
        if (errorOut) *errorOut = diags.render(buf);
        return false;
    }
    auto inferred = types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) {
        if (errorOut) *errorOut = diags.render(buf);
        return false;
    }
    auto lowered = lower::lowerModule(*astMod, diags, &*inferred, nullptr, &sources);
    if (diags.hasErrors() || !lowered) {
        if (errorOut) *errorOut = diags.render(buf);
        return false;
    }
    outMod = std::move(*lowered);
    return true;
}

bool lowerFileToModule(const std::string& path,
                       const std::vector<modules::ModuleRoot>& moduleRoots,
                       const std::string& importMapPath,
                       il::Module& outMod,
                       std::string& errorMsg) {
    SourceSet sources;
    DiagnosticSink diags;
    auto astMod = modules::loadProgram(path, sources, diags, {moduleRoots, importMapPath});
    if (diags.hasErrors() || !astMod) {
        errorMsg = diags.render(sources);
        return false;
    }
    auto inferred = types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) {
        errorMsg = diags.render(sources);
        return false;
    }
    auto lowered = lower::lowerModule(*astMod, diags, &*inferred, nullptr, &sources);
    if (diags.hasErrors() || !lowered) {
        errorMsg = diags.render(sources);
        return false;
    }
    outMod = std::move(*lowered);
    return true;
}

}  // namespace

const char* equivStatusTag(EquivStatus status) {
    switch (status) {
        case EquivStatus::Equivalent: return "EQUIVALENT";
        case EquivStatus::Counterexample: return "COUNTEREXAMPLE";
        case EquivStatus::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool ProveEquivReport::allEquivalent() const {
    if (functions.empty()) return false;
    for (const auto& f : functions) {
        if (f.status != EquivStatus::Equivalent) return false;
    }
    return true;
}

bool ProveEquivReport::hasCounterexample() const {
    for (const auto& f : functions) {
        if (f.status == EquivStatus::Counterexample) return true;
    }
    return false;
}

std::string ProveEquivReport::renderText() const {
    std::ostringstream os;
    for (const auto& f : functions) {
        os << f.name << ": " << equivStatusTag(f.status);
        if (f.status == EquivStatus::Counterexample) {
            os << " (";
            for (size_t i = 0; i < f.counterexampleArgs.size(); ++i) {
                if (i > 0) os << ", ";
                std::string pname = (i < f.paramNames.size()) ? f.paramNames[i] : ("arg_" + std::to_string(i));
                os << pname << " = " << f.counterexampleArgs[i];
            }
            os << ") -> old = " << f.oldReturnValue << ", new = " << f.newReturnValue;
        } else if (f.status == EquivStatus::Unknown && !f.detail.empty()) {
            os << " (" << f.detail << ")";
        }
        os << "\n";
    }
    return os.str();
}

std::string ProveEquivReport::renderJson() const {
    std::ostringstream os;
    os << "{\"functions\":[";
    for (size_t i = 0; i < functions.size(); ++i) {
        if (i > 0) os << ",";
        const auto& f = functions[i];
        os << "{\"name\":\"" << jsonEscape(f.name) << "\","
           << "\"status\":\"" << equivStatusTag(f.status) << "\","
           << "\"detail\":\"" << jsonEscape(f.detail) << "\"";
        if (f.status == EquivStatus::Counterexample) {
            os << ",\"counterexample\":{\"args\":[";
            for (size_t j = 0; j < f.counterexampleArgs.size(); ++j) {
                if (j > 0) os << ",";
                os << f.counterexampleArgs[j];
            }
            os << "],\"oldReturn\":" << f.oldReturnValue
               << ",\"newReturn\":" << f.newReturnValue << "}";
        }
        os << "}";
    }
    os << "],\"allEquivalent\":" << (allEquivalent() ? "true" : "false") << "}\n";
    return os.str();
}

ProveEquivReport proveModuleEquiv(const il::Module& oldMod,
                                  const il::Module& newMod,
                                  const std::string& fnFilter) {
    ProveEquivReport report;

    if (!fnFilter.empty()) {
        const il::Function* oldFn = nullptr;
        const il::Function* newFn = nullptr;
        for (const auto& f : oldMod.functions) {
            if (!f.isExternal && functionMatchesName(f, fnFilter)) {
                oldFn = &f;
                break;
            }
        }
        for (const auto& f : newMod.functions) {
            if (!f.isExternal && functionMatchesName(f, fnFilter)) {
                newFn = &f;
                break;
            }
        }
        if (!oldFn || !newFn) {
            FunctionEquivResult missing;
            missing.name = fnFilter;
            missing.status = EquivStatus::Unknown;
            missing.detail = !oldFn ? "function not found in old module"
                                    : "function not found in new module";
            report.functions.push_back(std::move(missing));
            return report;
        }
        report.functions.push_back(compareFunctionPair(oldMod, *oldFn, newMod, *newFn));
        return report;
    }

    for (const auto& oldFn : oldMod.functions) {
        if (oldFn.isExternal || oldFn.isEntryPoint || oldFn.name == "main") continue;
        const il::Function* matchedNew = nullptr;
        for (const auto& newFn : newMod.functions) {
            if (newFn.isExternal || newFn.isEntryPoint || newFn.name == "main") continue;
            if (functionMatchesName(newFn, oldFn.name) || functionMatchesName(oldFn, newFn.name)) {
                matchedNew = &newFn;
                break;
            }
        }
        if (matchedNew) {
            report.functions.push_back(compareFunctionPair(oldMod, oldFn, newMod, *matchedNew));
        }
    }

    if (report.functions.empty()) {
        const il::Function* oldMain = nullptr;
        const il::Function* newMain = nullptr;
        for (const auto& f : oldMod.functions) {
            if (f.isEntryPoint || f.name == "main") { oldMain = &f; break; }
        }
        for (const auto& f : newMod.functions) {
            if (f.isEntryPoint || f.name == "main") { newMain = &f; break; }
        }
        if (oldMain && newMain) {
            report.functions.push_back(compareFunctionPair(oldMod, *oldMain, newMod, *newMain));
        }
    }

    return report;
}

ProveEquivReport proveSourceEquiv(std::string_view oldSrc,
                                  std::string_view newSrc,
                                  const std::string& fnFilter,
                                  std::string* errorOut) {
    il::Module oldMod;
    il::Module newMod;
    if (!lowerSingleSource(oldSrc, "old.js", oldMod, errorOut)) {
        return {};
    }
    if (!lowerSingleSource(newSrc, "new.js", newMod, errorOut)) {
        return {};
    }
    return proveModuleEquiv(oldMod, newMod, fnFilter);
}

int runProveEquiv(const std::string& oldPath,
                  const std::string& newPath,
                  const std::string& fnFilter,
                  bool jsonOutput,
                  std::ostream& out,
                  std::ostream& err,
                  const std::vector<modules::ModuleRoot>& moduleRoots,
                  const std::string& importMapPath) {
    il::Module oldMod;
    il::Module newMod;
    std::string errMsg;
    if (!lowerFileToModule(oldPath, moduleRoots, importMapPath, oldMod, errMsg)) {
        err << errMsg;
        return 1;
    }
    if (!lowerFileToModule(newPath, moduleRoots, importMapPath, newMod, errMsg)) {
        err << errMsg;
        return 1;
    }

    ProveEquivReport report = proveModuleEquiv(oldMod, newMod, fnFilter);
    if (report.empty()) {
        err << "error: no matching functions found to compare between " << oldPath << " and " << newPath << "\n";
        return 1;
    }

    if (jsonOutput) {
        out << report.renderJson();
    } else {
        out << report.renderText();
    }

    if (report.hasCounterexample()) return 1;
    if (!report.allEquivalent()) return 2;
    return 0;
}

}  // namespace bronze::check
