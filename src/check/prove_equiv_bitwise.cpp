#include "check/prove_equiv.h"
#include "check/prove_equiv_heap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <verify/bv_solver.hpp>

namespace bronze::check {
namespace {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;
using brass::verify::SolveOutcome;

constexpr uint32_t kMaxBlockSteps = 110;
constexpr uint32_t kMaxTotalBlockVisits = 1024;
constexpr uint32_t kMaxInlineDepth = 6;

struct SymState {
    std::unordered_map<il::ValueId, BVNodeId> values;
    std::unordered_map<uint64_t, BVNodeId> envSlots;
    std::unordered_map<uint32_t, std::string> globalRefs;
    std::unordered_map<il::ValueId, std::vector<BVNodeId>> arrayValues;
    std::unordered_map<uint64_t, std::vector<BVNodeId>> envArrays;
    SymbolicHeapState heap;
};

struct SymEvalOutcome {
    bool supported = true;
    std::string unsupportedReason;
    bool hasReturn = false;
    BVNodeId returnVal = 0;
    SymbolicHeapState finalHeap;
    std::string returnObjPath;
};

class SymbolicIlBuilder {
public:
    SymbolicIlBuilder(BVContext& ctx,
                      const il::Module& mod,
                      bool strictJs32Bitwise = false,
                      SymbolicHeapTable* sharedHeapTable = nullptr)
        : ctx_(ctx),
          mod_(mod),
          strictJs32Bitwise_(strictJs32Bitwise),
          ownedHeapTable_(ctx),
          heapTable_(sharedHeapTable ? *sharedHeapTable : ownedHeapTable_) {
        zero64_ = ctx_.make_const(0, 64);
        one64_ = ctx_.make_const(1, 64);
        allOnes64_ = ctx_.make_const(~uint64_t{0}, 64);
    }

    SymEvalOutcome evalFunction(const il::Function& fn,
                                const std::vector<BVNodeId>& argNodes,
                                uint32_t inlineDepth = 0,
                                const SymbolicHeapState* callerHeap = nullptr,
                                const std::string& thisPath = "this",
                                const std::vector<std::string>* callerArgPaths = nullptr) {
        if (fn.blocks.empty() || fn.isExternal) {
            return {false, "external or empty function", false, zero64_, {}, {}};
        }
        if (inlineDepth > kMaxInlineDepth) {
            return {false, "call recursion depth exceeded", false, zero64_, {}, {}};
        }

        SymState initState;
        if (callerHeap) {
            initState.heap.writes = callerHeap->writes;
            initState.heap.localObjCounter = callerHeap->localObjCounter;
        }
        uint32_t firstSrc = static_cast<uint32_t>(fn.firstSourceParam());
        for (uint32_t i = 0; i < firstSrc; ++i) {
            initState.values[i] = zero64_;
        }
        uint32_t srcCount = static_cast<uint32_t>(fn.callerParamCount());
        for (uint32_t i = 0; i < srcCount; ++i) {
            BVNodeId arg = (i < argNodes.size()) ? argNodes[i] : zero64_;
            initState.values[firstSrc + i] = arg;
        }
        initState.heap.initFunctionHeap(heapTable_, fn, initState.values, thisPath, callerArgPaths);

        return evalBlockPath(fn, 0, initState, 0, inlineDepth);
    }

private:
    BVContext& ctx_;
    const il::Module& mod_;
    bool strictJs32Bitwise_ = false;
    SymbolicHeapTable ownedHeapTable_;
    SymbolicHeapTable& heapTable_;
    BVNodeId zero64_ = 0;
    BVNodeId one64_ = 0;
    BVNodeId allOnes64_ = 0;
    uint32_t totalVisits_ = 0;
    bool hitLoopUnrollBound_ = false;

public:
    bool hitLoopUnrollBound() const { return hitLoopUnrollBound_; }

private:

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

    BVNodeId toSInt32In64(BVNodeId node) {
        BVNodeId v32 = ctx_.make_extract(toBV64(node), 31, 0);
        return ctx_.make_sext(v32, 32);
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

    BVNodeId evalPow(BVNodeId base64, BVNodeId exp64, bool& ok) {
        ok = true;
        const auto& bNode = ctx_.nodes()[base64];
        const auto& eNode = ctx_.nodes()[exp64];
        if (bNode.op == BVOp::Const && static_cast<int64_t>(bNode.imm) == 2) {
            BVNodeId clamped = ctx_.make_binary(BVOp::And, exp64, ctx_.make_const(63, 64));
            return ctx_.make_binary(BVOp::Shl, one64_, clamped);
        }
        if (eNode.op == BVOp::Const) {
            int64_t expVal = static_cast<int64_t>(eNode.imm);
            if (expVal >= 0 && expVal <= 16) {
                BVNodeId acc = one64_;
                for (int64_t i = 0; i < expVal; ++i) {
                    acc = ctx_.make_binary(BVOp::Mul, acc, base64);
                }
                return acc;
            }
        }
        ok = false;
        return zero64_;
    }

    void applyEdgeArgsInPlace(const il::Function& fn, const il::BlockTarget& edge, SymState& state) {
        if (edge.block < fn.blocks.size()) {
            const auto& targetBlk = fn.blocks[edge.block];
            std::vector<std::pair<il::ValueId, BVNodeId>> updates;
            updates.reserve(targetBlk.params.size());
            for (size_t k = 0; k < targetBlk.params.size() && k < edge.args.size(); ++k) {
                updates.emplace_back(targetBlk.params[k].id, toBV64(getVal(state, edge.args[k])));
                state.heap.propagateAlias(edge.args[k], targetBlk.params[k].id);
            }
            for (const auto& [dst, val] : updates) {
                state.values[dst] = val;
            }
        }
    }

    SymEvalOutcome evalBlockPath(const il::Function& fn,
                                 il::BlockId blockId,
                                 SymState state,
                                 uint32_t steps,
                                 uint32_t inlineDepth) {
        while (true) {
            if (++totalVisits_ > kMaxTotalBlockVisits) {
                return {false, "symbolic path limit exceeded", false, zero64_};
            }
            if (steps > kMaxBlockSteps) {
                hitLoopUnrollBound_ = true;
                return {true, "loop unroll bound reached", false, zero64_};
            }
            if (blockId >= fn.blocks.size()) {
                return {false, "invalid block target", false, zero64_};
            }

            bool jumpedInPlace = false;
            const il::Block& blk = fn.blocks[blockId];
            for (size_t instIdx = 0; instIdx < blk.instructions.size(); ++instIdx) {
            const auto& inst = blk.instructions[instIdx];
            using Op = il::Op;
            switch (inst.op) {
                case Op::ConstI32:
                    state.values[inst.result] = ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immI32)), 64);
                    break;
                case Op::ConstF64: {
                    if (std::isnan(inst.immF64) || std::isinf(inst.immF64)) {
                        return {false, "non-finite floating-point constant", false, zero64_};
                    }
                    if (std::trunc(inst.immF64) != inst.immF64) {
                        return {false, "fractional floating-point constant", false, zero64_};
                    }
                    state.values[inst.result] = ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immF64)), 64);
                    break;
                }
                case Op::ConstBool:
                    state.values[inst.result] = (inst.immI32 != 0) ? one64_ : zero64_;
                    break;
                case Op::ConstNull:
                case Op::ConstUndefined:
                case Op::IsNullish:
                    state.values[inst.result] = zero64_;
                    break;
                case Op::ToInt32: {
                    BVNodeId v = inst.operands.empty() ? zero64_ : getVal(state, inst.operands[0]);
                    state.values[inst.result] = toSInt32In64(v);
                    break;
                }
                case Op::Box:
                case Op::Unbox:
                case Op::ToNumeric:
                case Op::NumTruthy: {
                    if (!inst.operands.empty()) {
                        state.values[inst.result] = toBV64(getVal(state, inst.operands[0]));
                        state.heap.propagateAlias(inst.operands[0], inst.result);
                        auto arrIt = state.arrayValues.find(inst.operands[0]);
                        if (arrIt != state.arrayValues.end()) state.arrayValues[inst.result] = arrIt->second;
                    } else {
                        state.values[inst.result] = zero64_;
                    }
                    break;
                }
                case Op::PinGuard:
                case Op::CensusRecord:
                case Op::FeedbackRecord:
                case Op::KeepAlive:
                    break;
                case Op::IsNumber:
                case Op::IsDenseArray:
                case Op::IsNumberArray:
                case Op::IsShape:
                case Op::IsArray:
                case Op::IsTypedArray:
                case Op::LayoutEpoch:
                case Op::LayoutFact:
                case Op::MethodIcIs:
                case Op::AccessorIcIs:
                    if (inst.result != il::kNoValue) state.values[inst.result] = one64_;
                    break;
                case Op::CreateObject:
                    state.values[inst.result] = state.heap.createLocalObject(heapTable_, inst.result);
                    break;
                case Op::PropGet: {
                    if (inst.operands.empty() || inst.keyIndex >= mod_.keyConstants.size()) {
                        return {false, "malformed PropGet", false, zero64_, {}, {}};
                    }
                    const std::string& key = mod_.keyConstants[inst.keyIndex];
                    auto arrIt = state.arrayValues.find(inst.operands[0]);
                    if (arrIt != state.arrayValues.end() && key == "length") {
                        state.values[inst.result] = ctx_.make_const(arrIt->second.size(), 64);
                        break;
                    }
                    BVNodeId propVal = zero64_;
                    if (!state.heap.evalPropGet(heapTable_, inst.operands[0], key, inst.result, propVal)) {
                        return {false, "unsupported PropGet on non-tracked receiver", false, zero64_, {}, {}};
                    }
                    state.values[inst.result] = propVal;
                    break;
                }
                case Op::PropSet:
                case Op::DefineOwnAttr: {
                    if (inst.operands.size() < 2 || inst.keyIndex >= mod_.keyConstants.size()) {
                        return {false, "malformed PropSet", false, zero64_, {}, {}};
                    }
                    const std::string& key = mod_.keyConstants[inst.keyIndex];
                    BVNodeId valNode = toBV64(getVal(state, inst.operands[1]));
                    if (!state.heap.evalPropSet(heapTable_, inst.operands[0], key, valNode, inst.operands[1])) {
                        return {false, "unsupported PropSet on non-tracked receiver", false, zero64_, {}, {}};
                    }
                    if (inst.result != il::kNoValue) state.values[inst.result] = valNode;
                    break;
                }
                case Op::NumericStep:
                    state.values[inst.result] = ctx_.make_binary(
                        BVOp::Add, toBV64(getVal(state, inst.operands[0])),
                        ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immI32)), 64));
                    break;
                case Op::MathImul: {
                    BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[0])), 31, 0);
                    BVNodeId b32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[1])), 31, 0);
                    state.values[inst.result] = ctx_.make_sext(ctx_.make_binary(BVOp::Mul, a32, b32), 32);
                    break;
                }
                case Op::Add:
                case Op::Sub:
                case Op::Mul: {
                    BVOp bvOp = (inst.op == Op::Add) ? BVOp::Add : (inst.op == Op::Sub ? BVOp::Sub : BVOp::Mul);
                    state.values[inst.result] = ctx_.make_binary(
                        bvOp, toBV64(getVal(state, inst.operands[0])), toBV64(getVal(state, inst.operands[1])));
                    break;
                }
                case Op::Pow: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    bool ok = false;
                    BVNodeId r = evalPow(a, b, ok);
                    if (!ok) return {false, "unsupported symbolic exponentiation", false, zero64_};
                    state.values[inst.result] = r;
                    break;
                }
                case Op::Div:
                case Op::Mod: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    BVNodeId divVal = zero64_;
                    std::string divErr;
                    if (!evalDivOrModSymbolic(ctx_, mod_, blk, instIdx, a, b, state.heap, divVal, &divErr)) {
                        return {false, divErr, false, zero64_, {}, {}};
                    }
                    state.values[inst.result] = divVal;
                    break;
                }
                case Op::Neg: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    state.values[inst.result] = ctx_.make_unary(BVOp::Neg, a);
                    break;
                }
                case Op::BitAnd: {
                    BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[0])), 31, 0);
                    BVNodeId b32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[1])), 31, 0);
                    BVNodeId r32 = ctx_.make_binary(BVOp::And, a32, b32);
                    state.values[inst.result] = ctx_.make_sext(r32, 32);
                    break;
                }
                case Op::BitOr: {
                    BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[0])), 31, 0);
                    BVNodeId b32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[1])), 31, 0);
                    BVNodeId r32 = ctx_.make_binary(BVOp::Or, a32, b32);
                    state.values[inst.result] = ctx_.make_sext(r32, 32);
                    break;
                }
                case Op::BitXor: {
                    BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[0])), 31, 0);
                    BVNodeId b32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[1])), 31, 0);
                    BVNodeId r32 = ctx_.make_binary(BVOp::Xor, a32, b32);
                    state.values[inst.result] = ctx_.make_sext(r32, 32);
                    break;
                }
                case Op::BitNot: {
                    BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[0])), 31, 0);
                    BVNodeId r32 = ctx_.make_unary(BVOp::Not, a32);
                    state.values[inst.result] = ctx_.make_sext(r32, 32);
                    break;
                }
                case Op::Shl: {
                    BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[0])), 31, 0);
                    BVNodeId b32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[1])), 31, 0);
                    BVNodeId mask31 = ctx_.make_const(31, 32);
                    BVNodeId sh32 = ctx_.make_binary(BVOp::And, b32, mask31);
                    BVNodeId r32 = ctx_.make_binary(BVOp::Shl, a32, sh32);
                    state.values[inst.result] = ctx_.make_sext(r32, 32);
                    break;
                }
                case Op::Shr: {
                    BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[0])), 31, 0);
                    BVNodeId b32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[1])), 31, 0);
                    BVNodeId mask31 = ctx_.make_const(31, 32);
                    BVNodeId sh32 = ctx_.make_binary(BVOp::And, b32, mask31);
                    BVNodeId r32 = ctx_.make_binary(BVOp::AShr, a32, sh32);
                    state.values[inst.result] = ctx_.make_sext(r32, 32);
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
                    state.heap.recordObjComparison(inst.result, inst.operands[0], inst.operands[1], true);
                    break;
                }
                case Op::CmpNe: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = toBV64(ctx_.make_binary(BVOp::Ne, a, b));
                    state.heap.recordObjComparison(inst.result, inst.operands[0], inst.operands[1], false);
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
                case Op::CreateArray: {
                    state.values[inst.result] = zero64_;
                    state.arrayValues[inst.result] = {};
                    break;
                }
                case Op::ArrayAppend: {
                    if (inst.operands.size() >= 2) {
                        state.arrayValues[inst.operands[0]].push_back(toBV64(getVal(state, inst.operands[1])));
                    }
                    break;
                }
                case Op::ElemGet:
                case Op::ElemGetTyped: {
                    if (inst.operands.size() < 2) {
                        return {false, "malformed ElemGet", false, zero64_, {}, {}};
                    }
                    auto arrIt = state.arrayValues.find(inst.operands[0]);
                    if (arrIt != state.arrayValues.end() && !arrIt->second.empty()) {
                        const auto& elems = arrIt->second;
                        BVNodeId idx = toBV64(getVal(state, inst.operands[1]));
                        BVNodeId res = zero64_;
                        for (size_t i = 0; i < elems.size(); ++i) {
                            BVNodeId eq = ctx_.make_binary(BVOp::Eq, idx, ctx_.make_const(i, 64));
                            res = ctx_.make_ite(eq, elems[i], res);
                        }
                        state.values[inst.result] = res;
                        break;
                    }
                    BVNodeId elemVal = zero64_;
                    BVNodeId idxNode = toBV64(getVal(state, inst.operands[1]));
                    if (!state.heap.evalElemGet(heapTable_, inst.operands[0], idxNode, inst.result, elemVal)) {
                        return {false, "unsupported dynamic array ElemGet", false, zero64_, {}, {}};
                    }
                    state.values[inst.result] = elemVal;
                    break;
                }
                case Op::ElemSet:
                case Op::ElemSetTyped: {
                    if (inst.operands.size() < 3) {
                        return {false, "malformed ElemSet", false, zero64_, {}, {}};
                    }
                    BVNodeId idxNode = toBV64(getVal(state, inst.operands[1]));
                    BVNodeId valNode = toBV64(getVal(state, inst.operands[2]));
                    if (!state.heap.evalElemSet(heapTable_, inst.operands[0], idxNode, valNode, inst.operands[2])) {
                        return {false, "unsupported ElemSet on non-tracked receiver", false, zero64_, {}, {}};
                    }
                    if (inst.result != il::kNoValue) {
                        state.values[inst.result] = valNode;
                    }
                    break;
                }
                case Op::MathUnary: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    auto kind = static_cast<il::MathUnaryFn>(inst.immI32);
                    if (kind == il::MathUnaryFn::Abs) {
                        BVNodeId isNeg = ctx_.make_binary(BVOp::Slt, a, zero64_);
                        BVNodeId negA = ctx_.make_unary(BVOp::Neg, a);
                        state.values[inst.result] = ctx_.make_ite(isNeg, negA, a);
                    } else if (kind == il::MathUnaryFn::Floor || kind == il::MathUnaryFn::Ceil) {
                        state.values[inst.result] = evalFloorOrCeilSymbolic(
                            ctx_, state.heap, inst.operands[0], a, kind == il::MathUnaryFn::Ceil);
                    } else if (kind == il::MathUnaryFn::Trunc) {
                        state.values[inst.result] = a;
                    } else if (kind == il::MathUnaryFn::Min || kind == il::MathUnaryFn::Max) {
                        BVNodeId acc = a;
                        for (size_t k = 1; k < inst.operands.size(); ++k) {
                            BVNodeId rhs = toBV64(getVal(state, inst.operands[k]));
                            BVNodeId pick = kind == il::MathUnaryFn::Min
                                                ? ctx_.make_binary(BVOp::Slt, rhs, acc)
                                                : ctx_.make_binary(BVOp::Slt, acc, rhs);
                            acc = ctx_.make_ite(pick, rhs, acc);
                        }
                        state.values[inst.result] = acc;
                    } else {
                        return {false, "unsupported transcendental MathUnary", false, zero64_, {}, {}};
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
                        return {false, "unsupported dynamic method call", false, zero64_, {}, {}};
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
                        if (method == "log2" && inst.operands.size() >= 2) {
                            BVNodeId a = toBV64(getVal(state, inst.operands[1]));
                            BVNodeId c31 = ctx_.make_const(31, 64);
                            state.values[inst.result] = ctx_.make_binary(BVOp::Sub, c31, makeClz32(a));
                            break;
                        }
                        if (method == "pow" && inst.operands.size() >= 3) {
                            BVNodeId a = toBV64(getVal(state, inst.operands[1]));
                            BVNodeId b = toBV64(getVal(state, inst.operands[2]));
                            bool ok = false;
                            BVNodeId r = evalPow(a, b, ok);
                            if (!ok) return {false, "unsupported symbolic Math.pow", false, zero64_, {}, {}};
                            state.values[inst.result] = r;
                            break;
                        }
                        if ((method == "floor" || method == "ceil") && inst.operands.size() >= 2) {
                            BVNodeId argVal = toBV64(getVal(state, inst.operands[1]));
                            state.values[inst.result] = evalFloorOrCeilSymbolic(
                                ctx_, state.heap, inst.operands[1], argVal, method == "ceil");
                            break;
                        }
                        if ((method == "trunc" || method == "round" || method == "fround") &&
                            inst.operands.size() >= 2) {
                            state.values[inst.result] = toBV64(getVal(state, inst.operands[1]));
                            break;
                        }
                    }
                    if (recvIt != state.globalRefs.end() && recvIt->second == "Number" &&
                        inst.operands.size() >= 2) {
                        if (method == "isFinite" || method == "isSafeInteger" || method == "isInteger") {
                            state.values[inst.result] = one64_;
                            break;
                        }
                        if (method == "isNaN") {
                            state.values[inst.result] = zero64_;
                            break;
                        }
                    }
                    // Direct or class method call on a tracked object receiver (`this.set(...)`, `v.dot(...)`)
                    std::string recvPath = state.heap.lookupObjPath(inst.operands[0]);
                    if (!recvPath.empty()) {
                        const il::Function* callee = nullptr;
                        if (inst.directTarget != il::Instruction::kNoDirectTarget &&
                            inst.directTarget < mod_.functions.size()) {
                            callee = &mod_.functions[inst.directTarget];
                        } else {
                            std::string dotSuffix = "." + method;
                            for (const auto& cand : mod_.functions) {
                                if (cand.isExternal || cand.isEntryPoint) continue;
                                if (cand.name == method || cand.displayName == method ||
                                    (cand.displayName.size() > dotSuffix.size() &&
                                     cand.displayName.compare(cand.displayName.size() - dotSuffix.size(),
                                                              dotSuffix.size(), dotSuffix) == 0)) {
                                    callee = &cand;
                                    break;
                                }
                            }
                        }
                        if (callee && !callee->isExternal) {
                            std::vector<BVNodeId> callArgs;
                            std::vector<std::string> callPaths;
                            for (size_t k = 1; k < inst.operands.size(); ++k) {
                                callArgs.push_back(toBV64(getVal(state, inst.operands[k])));
                                callPaths.push_back(state.heap.lookupObjPath(inst.operands[k]));
                            }
                            auto sub = evalFunction(*callee, callArgs, inlineDepth + 1,
                                                    &state.heap, recvPath, &callPaths);
                            if (!sub.supported) return sub;
                            state.heap.writes = std::move(sub.finalHeap.writes);
                            state.heap.localObjCounter = sub.finalHeap.localObjCounter;
                            state.values[inst.result] = sub.returnVal;
                            if (!sub.returnObjPath.empty() && inst.result != il::kNoValue) {
                                state.heap.objPaths[inst.result] = sub.returnObjPath;
                            }
                            break;
                        }
                    }
                    return {false, "unsupported method call: " + method, false, zero64_, {}, {}};
                }
                case Op::Call: {
                    if (inst.calleeIndex >= mod_.functions.size()) {
                        return {false, "invalid callee index", false, zero64_, {}, {}};
                    }
                    const il::Function& callee = mod_.functions[inst.calleeIndex];
                    std::vector<BVNodeId> callArgs;
                    std::vector<std::string> callPaths;
                    size_t startOp = (inst.callEnvHops != il::Instruction::kNoEnvHops && !inst.operands.empty()) ? 1 : 0;
                    for (size_t k = startOp; k < inst.operands.size(); ++k) {
                        callArgs.push_back(toBV64(getVal(state, inst.operands[k])));
                        callPaths.push_back(state.heap.lookupObjPath(inst.operands[k]));
                    }
                    auto sub = evalFunction(callee, callArgs, inlineDepth + 1,
                                            &state.heap, "this", &callPaths);
                    if (!sub.supported) return sub;
                    state.heap.writes = std::move(sub.finalHeap.writes);
                    state.heap.localObjCounter = sub.finalHeap.localObjCounter;
                    state.values[inst.result] = sub.returnVal;
                    if (!sub.returnObjPath.empty() && inst.result != il::kNoValue) {
                        state.heap.objPaths[inst.result] = sub.returnObjPath;
                    }
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
                        state.heap.propagateEnvSet(key, inst.operands[1]);
                        auto arrIt = state.arrayValues.find(inst.operands[1]);
                        if (arrIt != state.arrayValues.end()) {
                            state.envArrays[key] = arrIt->second;
                        }
                    }
                    break;
                }
                case Op::EnvGet:
                case Op::EnvGetTdz: {
                    uint64_t key = (static_cast<uint64_t>(inst.envDepth) << 32) | inst.envIndex;
                    auto it = state.envSlots.find(key);
                    state.values[inst.result] = (it != state.envSlots.end()) ? it->second : zero64_;
                    state.heap.propagateEnvGet(key, inst.result);
                    auto arrIt = state.envArrays.find(key);
                    if (arrIt != state.envArrays.end()) {
                        state.arrayValues[inst.result] = arrIt->second;
                    }
                    break;
                }
                case Op::EnvInitTdz:
                    break;
                case Op::Jump: {
                    applyEdgeArgsInPlace(fn, inst.target, state);
                    blockId = inst.target.block;
                    ++steps;
                    jumpedInPlace = true;
                    break;
                }
                case Op::Branch: {
                    BVNodeId condNode = toBool1(toBV64(getVal(state, inst.operands[0])));
                    const auto& cNode = ctx_.nodes()[condNode];

                    if (cNode.op == BVOp::Const) {
                        const il::BlockTarget& edge = (cNode.imm != 0) ? inst.target : inst.elseTarget;
                        applyEdgeArgsInPlace(fn, edge, state);
                        blockId = edge.block;
                        ++steps;
                        jumpedInPlace = true;
                        break;
                    }

                    SymState trueState = state;
                    trueState.heap.unifyBranchAlias(heapTable_, inst.operands[0], condNode, true);
                    applyEdgeArgsInPlace(fn, inst.target, trueState);
                    auto trueOut = evalBlockPath(fn, inst.target.block, std::move(trueState), steps + 1, inlineDepth);
                    if (!trueOut.supported) return trueOut;

                    state.heap.unifyBranchAlias(heapTable_, inst.operands[0], condNode, false);
                    applyEdgeArgsInPlace(fn, inst.elseTarget, state);
                    auto falseOut = evalBlockPath(fn, inst.elseTarget.block, std::move(state), steps + 1, inlineDepth);
                    if (!falseOut.supported) return falseOut;

                    if (trueOut.hasReturn && falseOut.hasReturn) {
                        BVNodeId merged = ctx_.make_ite(condNode, trueOut.returnVal, falseOut.returnVal);
                        SymbolicHeapState mergedHeap = SymbolicHeapState::mergeBranches(
                            heapTable_, condNode, trueOut.finalHeap, falseOut.finalHeap);
                        std::string retObj = (trueOut.returnObjPath == falseOut.returnObjPath)
                                                 ? trueOut.returnObjPath
                                                 : std::string{};
                        return {true, {}, true, merged, std::move(mergedHeap), std::move(retObj)};
                    }
                    if (trueOut.hasReturn) return trueOut;
                    if (falseOut.hasReturn) return falseOut;
                    return {true, "loop unroll bound reached", false, zero64_, {}, {}};
                }
                case Op::Ret: {
                    BVNodeId ret = inst.operands.empty() ? zero64_ : toBV64(getVal(state, inst.operands[0]));
                    std::string retPath = inst.operands.empty()
                                              ? std::string{}
                                              : state.heap.lookupObjPath(inst.operands[0]);
                    return {true, {}, true, ret, std::move(state.heap), std::move(retPath)};
                }
                default:
                    return {false, std::string("unsupported IL operation: ") + il::opName(inst.op), false, zero64_, {}, {}};
            }
            if (jumpedInPlace) break;
        }
        if (!jumpedInPlace) {
            return {false, "block terminated without Ret/Jump/Branch", false, zero64_, {}, {}};
        }
        }
    }
};

}  // namespace

bool evalIlFunctionSymbolic(brass::verify::BVContext& ctx,
                            const il::Module& mod,
                            const il::Function& fn,
                            const std::vector<brass::verify::BVNodeId>& argNodes,
                            bool strictJs32Bitwise,
                            brass::verify::BVNodeId* outRetNode,
                            std::string* outReason,
                            SymbolicHeapTable* sharedHeapTable,
                            SymbolicHeapState* outFinalHeap) {
    SymbolicIlBuilder builder(ctx, mod, strictJs32Bitwise, sharedHeapTable);
    auto out = builder.evalFunction(fn, argNodes);
    if (!out.supported || !out.hasReturn) {
        if (outReason) *outReason = out.unsupportedReason;
        return false;
    }
    if (outRetNode) *outRetNode = out.returnVal;
    if (outFinalHeap) *outFinalHeap = std::move(out.finalHeap);
    return true;
}

FunctionEquivResult compareIlFunctionPair(const il::Module& oldMod,
                                          const il::Function& oldFn,
                                          const il::Module& newMod,
                                          const il::Function& newFn,
                                          bool strictJs32Bitwise,
                                          int32_t domainMin,
                                          int32_t domainMax) {
    FunctionEquivResult res;
    res.name = !oldFn.displayName.empty() ? oldFn.displayName : oldFn.name;

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

    SymbolicHeapTable heapTable(ctx);
    SymbolicIlBuilder oldSym(ctx, oldMod, strictJs32Bitwise, &heapTable);
    auto oldOut = oldSym.evalFunction(oldFn, argNodes);
    SymbolicIlBuilder newSym(ctx, newMod, strictJs32Bitwise, &heapTable);
    auto newOut = newSym.evalFunction(newFn, argNodes);

    if (!oldOut.supported || !newOut.supported ||
        !oldOut.hasReturn || !newOut.hasReturn ||
        oldSym.hitLoopUnrollBound() || newSym.hitLoopUnrollBound()) {
        auto indRes = compareIlLoopFunctionPairInductive(
            oldMod, oldFn, newMod, newFn, strictJs32Bitwise, domainMin, domainMax);
        if (indRes.status != EquivStatus::Unknown) {
            return indRes;
        }
        if (!oldOut.supported) {
            res.status = EquivStatus::Unknown;
            res.detail = "old function not symbolically decidable: " + oldOut.unsupportedReason;
            return res;
        }
        if (!newOut.supported) {
            res.status = EquivStatus::Unknown;
            res.detail = "new function not symbolically decidable: " + newOut.unsupportedReason;
            return res;
        }
        if (!oldOut.hasReturn || !newOut.hasReturn || domainMax > 45) {
            return indRes;
        }
    }

    std::vector<std::string> checkedHeapPaths;
    BVNodeId heapMismatch = SymbolicHeapState::buildHeapMismatchCond(
        heapTable, oldOut.finalHeap, newOut.finalHeap, &checkedHeapPaths);

    if (oldOut.returnVal == newOut.returnVal && heapMismatch == 0) {
        res.status = EquivStatus::Equivalent;
        res.detail = "proven equivalent by structural BV hash-consing";
        return res;
    }

    BVNodeId badCond = 0;
    if (oldOut.returnVal != newOut.returnVal) {
        badCond = ctx.make_binary(BVOp::Ne, oldOut.returnVal, newOut.returnVal);
    }
    if (heapMismatch != 0) {
        badCond = (badCond == 0) ? heapMismatch : ctx.make_binary(BVOp::Or, badCond, heapMismatch);
    }

    BVNodeId aliasCongruence = heapTable.buildAliasCongruenceCond();
    if (aliasCongruence != 0) {
        badCond = ctx.make_binary(BVOp::And, badCond, aliasCongruence);
    }

    if (domainMin > INT32_MIN || domainMax < INT32_MAX) {
        BVNodeId minNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(domainMin)), 64);
        BVNodeId maxNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(domainMax)), 64);
        auto constrainNode = [&](BVNodeId n) {
            if (domainMin > INT32_MIN) {
                badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_binary(BVOp::Sge, n, minNode));
            }
            if (domainMax < INT32_MAX) {
                badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_binary(BVOp::Sle, n, maxNode));
            }
        };
        for (BVNodeId arg : argNodes) constrainNode(arg);
        for (BVNodeId hv : heapTable.varNodes()) constrainNode(hv);
    }

    SolveOutcome satRes = brass::verify::solve_nonzero(ctx, badCond);

    if (satRes.status == SolveOutcome::Unsat) {
        res.status = EquivStatus::Equivalent;
        res.detail = "proven equivalent across all symbolic inputs and post-states via CDCL bit-blasting";
        return res;
    }

    if (satRes.status == SolveOutcome::Sat) {
        res.status = EquivStatus::Counterexample;
        const size_t totalVars = numParams + heapTable.varNodes().size();
        std::vector<uint64_t> bestModel = satRes.model;
        if (bestModel.size() < totalVars) bestModel.resize(totalVars, 0);
        minimizeCounterexampleModel(ctx, badCond, totalVars, domainMin, domainMax, bestModel);

        for (size_t i = 0; i < totalVars; ++i) {
            int32_t signed32 = static_cast<int32_t>(static_cast<uint32_t>(bestModel[i]));
            bestModel[i] = static_cast<uint64_t>(static_cast<uint32_t>(signed32));
        }
        res.counterexampleArgs.resize(numParams, 0);
        for (uint32_t i = 0; i < numParams; ++i) {
            res.counterexampleArgs[i] = static_cast<int64_t>(static_cast<int32_t>(bestModel[i]));
        }
        if (res.counterexampleArgs.empty() && !heapTable.varNodes().empty()) {
            for (size_t i = 0; i < heapTable.varNodes().size(); ++i) {
                res.paramNames.push_back(heapTable.varPaths()[i]);
                res.counterexampleArgs.push_back(
                    static_cast<int64_t>(static_cast<int32_t>(bestModel[numParams + i])));
            }
        }
        int64_t oldRet = static_cast<int64_t>(ctx.eval_concrete(oldOut.returnVal, bestModel));
        int64_t newRet = static_cast<int64_t>(ctx.eval_concrete(newOut.returnVal, bestModel));
        if (oldRet == newRet && !checkedHeapPaths.empty()) {
            for (const std::string& p : checkedHeapPaths) {
                int64_t ov = static_cast<int64_t>(
                    ctx.eval_concrete(oldOut.finalHeap.getFinalValue(heapTable, p), bestModel));
                int64_t nv = static_cast<int64_t>(
                    ctx.eval_concrete(newOut.finalHeap.getFinalValue(heapTable, p), bestModel));
                if (ov != nv) {
                    oldRet = ov;
                    newRet = nv;
                    res.detail = "counterexample found where post-state property `" + p + "` diverges";
                    break;
                }
            }
        }
        res.oldReturnValue = oldRet;
        res.newReturnValue = newRet;
        if (res.detail.empty()) {
            res.detail = "counterexample found where old and new return values diverge";
        }
        return res;
    }

    res.status = EquivStatus::Unknown;
    res.detail = "solver returned UNKNOWN (unsupported non-linear op or resource bound)";
    return res;
}

}  // namespace bronze::check
