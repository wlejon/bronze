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

inline constexpr uint32_t kMaxBlockSteps = 240;
inline constexpr uint32_t kMaxTotalBlockVisits = 16384;
inline constexpr uint32_t kMaxInlineDepth = 6;
inline constexpr uint32_t kMaxModuleInitSteps = 12000;
inline constexpr uint32_t kMaxModuleInitVisits = 24000;

struct SymState {
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
                                bool isConstructCall = false);

    bool hitLoopUnrollBound() const { return hitLoopUnrollBound_; }
    ProofBounds boundsUsed() const { return {maxLoopBoundUsed_, maxArrayBoundUsed_, maxStringBoundUsed_}; }

private:
    brass::verify::BVContext& ctx_;
    const il::Module& mod_;
    bool strictJs32Bitwise_ = false;
    SymbolicHeapTable ownedHeapTable_;
    SymbolicHeapTable& heapTable_;
    brass::verify::BVNodeId zero1_ = 0;
    brass::verify::BVNodeId one1_ = 0;
    brass::verify::BVNodeId zero64_ = 0;
    brass::verify::BVNodeId one64_ = 0;
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

    SymEvalOutcome evalConstructOp(const il::Instruction& inst, uint32_t inlineDepth, SymState& state);
    SymEvalOutcome evalMethodCallOp(const il::Instruction& inst, uint32_t inlineDepth, SymState& state);
    SymEvalOutcome evalDynamicCallOp(const il::Instruction& inst, uint32_t inlineDepth, SymState& state);
    SymEvalOutcome evalDirectCallOp(const il::Instruction& inst, uint32_t inlineDepth, SymState& state);

    SymEvalOutcome evalBlockPath(const il::Function& fn,
                                 il::BlockId blockId,
                                 SymState state,
                                 uint32_t steps,
                                 uint32_t inlineDepth);
};

}  // namespace bronze::check
