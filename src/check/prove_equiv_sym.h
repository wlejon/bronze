#pragma once

#include "check/prove_equiv.h"
#include "check/prove_equiv_heap.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <verify/bv_solver.hpp>

namespace bronze::check {

// The refusals of SymbolicHeapTable::numericParams() mode.
inline constexpr const char* kNumberToStringReason =
    "string conversion of a number (String(x), a template literal) is not modelled";
inline constexpr const char* kNumberConcatReason =
    "string concatenation with a number is not modelled";

inline constexpr uint32_t kMaxBlockSteps = 240;
inline constexpr uint32_t kMaxTotalBlockVisits = 16384;
inline constexpr uint32_t kMaxInlineDepth = 6;
inline constexpr uint32_t kMaxModuleInitSteps = 12000;
inline constexpr uint32_t kMaxModuleInitVisits = 24000;

// A value's JS kind where the bit-vector alone cannot tell: `true`, `1` and
// (for `undefined` / `null`) `0` share one bit-vector each, yet differ under
// `===`. A kind is a 2-bit node over these constants; a value with no kind
// recorded is a number, and kNoKind marks an outcome that set none.
inline constexpr uint64_t kKindNumber = 0;
inline constexpr uint64_t kKindBool = 1;
inline constexpr uint64_t kKindNullish = 2;
inline constexpr uint32_t kKindWidth = 2;
inline constexpr brass::verify::BVNodeId kNoKind = UINT32_MAX;

struct SymState {
    // The kind of each value that is (or, on a merged path, may be) not a
    // number: what keeps `x > 3` from proving equal to `x > 3 ? 1 : 0`.
    std::unordered_map<il::ValueId, brass::verify::BVNodeId> valueKind;
    std::unordered_map<il::ValueId, brass::verify::BVNodeId> values;
    std::unordered_map<uint64_t, brass::verify::BVNodeId> envSlots;
    std::unordered_map<uint32_t, std::string> globalRefs;
    std::unordered_map<uint64_t, std::string> envGlobalRefs;
    std::unordered_map<il::ValueId, std::vector<brass::verify::BVNodeId>> arrayValues;
    std::unordered_map<uint64_t, std::vector<brass::verify::BVNodeId>> envArrays;
    std::unordered_map<il::ValueId, uint32_t> envIds;
    std::unordered_map<uint32_t, uint32_t> envParent;
    std::unordered_map<il::ValueId, uint32_t> funcRefs;
    std::unordered_map<il::ValueId, uint32_t> funcEnvs;
    std::unordered_map<uint64_t, uint32_t> envFuncRefs;
    std::unordered_map<uint64_t, uint32_t> envFuncEnvs;
    std::unordered_map<uint32_t, uint32_t> funcCapturedEnvs;
    std::unordered_map<std::string, uint32_t> protoToCtor;
    std::unordered_map<uint32_t, std::string> ctorToProto;
    std::unordered_map<uint32_t, uint32_t> funcOwnerCtor;
    uint32_t moduleCtorIdx = UINT32_MAX;
    std::unordered_map<il::ValueId, uint32_t> ctorOf;
    std::unordered_set<il::ValueId> undefValues;
    std::unordered_set<uint64_t> envUndef;
    std::unordered_map<il::BlockId, uint32_t> loopHeaderVisits;
    bool symbolicLoopActive = false;
    uint32_t nextEnvId = 1;
    bool isConstructCall = false;
    brass::verify::BVNodeId threwCond = 0;
    brass::verify::BVNodeId exceededCond = 0;
    SymbolicHeapState heap;
};

struct SymEvalOutcome {
    bool supported = true;
    std::string unsupportedReason;
    bool hasReturn = false;
    brass::verify::BVNodeId returnVal = 0;
    SymbolicHeapState finalHeap;
    std::string returnObjPath;
    brass::verify::BVNodeId threwNode = 0;
    brass::verify::BVNodeId exceededBoundNode = 0;
    uint32_t returnFuncRef = UINT32_MAX;
    uint32_t returnFuncEnv = 0;
    uint32_t returnCtorOf = UINT32_MAX;
    std::string returnGlobalRef;
    std::vector<brass::verify::BVNodeId> returnArray;
    brass::verify::BVNodeId returnKind = kNoKind;
};

class SymbolicIlBuilder {
public:
    SymbolicIlBuilder(brass::verify::BVContext& ctx,
                      const il::Module& mod,
                      bool strictJs32Bitwise = false,
                      SymbolicHeapTable* sharedHeapTable = nullptr);

    SymEvalOutcome evalFunction(const il::Function& fn,
                                const std::vector<brass::verify::BVNodeId>& argNodes,
                                uint32_t inlineDepth = 0,
                                const SymbolicHeapState* callerHeap = nullptr,
                                const std::string& thisPath = "this",
                                const std::vector<std::string>* callerArgPaths = nullptr,
                                uint32_t callerEnvId = 0,
                                const std::vector<bool>* callerUndefArgs = nullptr,
                                bool isConstructCall = false,
                                uint32_t callerThisCtor = UINT32_MAX,
                                const std::vector<uint32_t>* callerArgCtors = nullptr,
                                SymState* callerEnvState = nullptr);

    bool hitLoopUnrollBound() const { return hitLoopUnrollBound_; }
    ProofBounds boundsUsed() const { return {maxLoopBoundUsed_, maxArrayBoundUsed_, maxStringBoundUsed_}; }

private:
    brass::verify::BVContext& ctx_;
    const il::Module& mod_;
    bool strictJs32Bitwise_ = false;
    bool floatMode_ = false;
    SymbolicHeapTable ownedHeapTable_;
    SymbolicHeapTable& heapTable_;
    brass::verify::BVNodeId zero1_ = 0;
    brass::verify::BVNodeId one1_ = 0;
    brass::verify::BVNodeId zero64_ = 0;
    brass::verify::BVNodeId one64_ = 0;
    brass::verify::BVNodeId f64One_ = 0;
    brass::verify::BVNodeId allOnes64_ = 0;
    uint32_t totalVisits_ = 0;
    bool hitLoopUnrollBound_ = false;
    uint32_t maxLoopBoundUsed_ = 0;
    uint32_t maxArrayBoundUsed_ = 0;
    uint32_t maxStringBoundUsed_ = 0;
    bool inModuleInit_ = false;
    SymState moduleState_;

    void initModuleState();
    uint64_t resolveEnvSlotKey(const SymState& st, il::ValueId envVal, uint32_t depth, uint32_t index) const;
    brass::verify::BVNodeId getVal(const SymState& st, il::ValueId v) const;
    brass::verify::BVNodeId toBool1(brass::verify::BVNodeId node);
    brass::verify::BVNodeId toBV64(brass::verify::BVNodeId node);
    brass::verify::BVNodeId toSInt32In64(brass::verify::BVNodeId node);
    brass::verify::BVNodeId kindConst(uint64_t kind) const;
    brass::verify::BVNodeId kindOf(const SymState& st, il::ValueId v) const;
    void setKind(SymState& st, il::ValueId v, brass::verify::BVNodeId kind) const;
    brass::verify::BVNodeId returnKindOf(const SymEvalOutcome& out) const;
    brass::verify::BVNodeId mergeKinds(brass::verify::BVNodeId cond,
                                       brass::verify::BVNodeId tKind,
                                       brass::verify::BVNodeId fKind);
    void applyEdgeArgsInPlace(const il::Function& fn, const il::BlockTarget& edge, SymState& state);
    void saveModuleInitSnapshot(const SymState& state);

    SymEvalOutcome invokeCallee(const il::Function& callee,
                                const il::Instruction& inst,
                                size_t startOp,
                                const std::string& thisPath,
                                uint32_t envId,
                                uint32_t inlineDepth,
                                SymState& state,
                                bool writeScalarRet = true,
                                bool isConstructCall = false);

    bool tryEvalGlobalCall(const std::string& gName,
                           const std::string& method,
                           const il::Instruction& inst,
                           SymState& state,
                           std::string& outErr);

    SymEvalOutcome evalGlobalGetOp(const il::Instruction& inst, SymState& state);
    SymEvalOutcome evalConstructOp(const il::Instruction& inst, uint32_t inlineDepth, SymState& state);
    SymEvalOutcome evalMethodCallOp(const il::Instruction& inst, uint32_t inlineDepth, SymState& state);
    SymEvalOutcome evalDynamicCallOp(const il::Instruction& inst, uint32_t inlineDepth, SymState& state);
    SymEvalOutcome evalDirectCallOp(const il::Instruction& inst, uint32_t inlineDepth, SymState& state);

    void loadBoundedSeq(SymState& state, il::ValueId valId, bool asString, BoundedSeq& outSeq);
    void storeBoundedSeqToNewLocal(SymState& state, il::ValueId resultId, const BoundedSeq& seq);
    void storeBoundedSeqInPlace(SymState& state, il::ValueId recvId, const std::string& path, const BoundedSeq& seq);
    void evalBoxStringLiteral(const std::string& lit, il::ValueId resultId, SymState& state);
    void evalStringConcatOp(il::ValueId lhsId, il::ValueId rhsId, il::ValueId resultId, SymState& state);
    void evalArraySpreadOp(const il::Instruction& inst, SymState& state);
    SymEvalOutcome evalCallSpreadOp(const il::Instruction& inst, bool isMethodSpread, SymState& state);
    bool tryEvalSeqMethodCallOp(const std::string& method,
                                const il::Instruction& inst,
                                SymState& state,
                                SymEvalOutcome& outOutcome);
    void canonicalizeSeqReturn(il::ValueId retOpId,
                               SymState& state,
                               std::string& retPath,
                               brass::verify::BVNodeId& ret);

    SymEvalOutcome evalBlockPath(const il::Function& fn,
                                 il::BlockId blockId,
                                 SymState state,
                                 uint32_t steps,
                                 uint32_t inlineDepth);
};

}  // namespace bronze::check
