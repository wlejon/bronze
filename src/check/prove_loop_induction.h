#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <verify/bv_solver.hpp>

#include "check/prove_equiv.h"
#include "check/prove_equiv_heap.h"

namespace bronze::check {

struct LoopSymState {
    std::unordered_map<il::ValueId, brass::verify::BVNodeId> values;
    std::unordered_map<uint64_t, brass::verify::BVNodeId> envSlots;
    std::unordered_map<uint32_t, std::string> globalRefs;
    SymbolicHeapState heap;
};

struct SingleLoopModel {
    il::BlockId headerId = 0;
    LoopSymState preHeaderState;
    std::vector<brass::verify::BVNodeId> initParams;
    size_t ivIdx = 0;
    size_t accIdx = 0;
    bool hasScalarAcc = false;
    brass::verify::BVNodeId boundNode = 0;
    bool exitIsIdentity = false;
};

bool hasNonPow2ConstDivisor(const il::Function& fn);

class LoopIlEvaluator {
public:
    LoopIlEvaluator(brass::verify::BVContext& ctx,
                    const il::Module& mod,
                    bool strictJs32Bitwise,
                    SymbolicHeapTable* sharedHeapTable = nullptr);

    brass::verify::BVNodeId zero64() const { return zero64_; }
    brass::verify::BVNodeId one64() const { return one64_; }

    bool extractSingleLoop(const il::Function& fn,
                           const std::vector<brass::verify::BVNodeId>& argNodes,
                           SingleLoopModel& out);

    bool evalHeaderBranch(const il::Function& fn,
                          const SingleLoopModel& loop,
                          brass::verify::BVNodeId ivVal,
                          brass::verify::BVNodeId accVal,
                          LoopSymState& outSt,
                          const il::Instruction*& outTerm);

    bool evalStep(const il::Function& fn,
                  const SingleLoopModel& loop,
                  brass::verify::BVNodeId ivVal,
                  brass::verify::BVNodeId accVal,
                  brass::verify::BVNodeId* outNextAcc,
                  SymbolicHeapState* outStepHeap = nullptr);

    bool evalExit(const il::Function& fn,
                  const SingleLoopModel& loop,
                  brass::verify::BVNodeId ivVal,
                  brass::verify::BVNodeId accVal,
                  brass::verify::BVNodeId* outRetVal);

private:
    brass::verify::BVContext& ctx_;
    const il::Module& mod_;
    bool strictJs32Bitwise_ = false;
    SymbolicHeapTable ownedHeapTable_;
    SymbolicHeapTable& heapTable_;
    brass::verify::BVNodeId zero64_ = 0;
    brass::verify::BVNodeId one64_ = 0;
    brass::verify::BVNodeId allOnes64_ = 0;

    brass::verify::BVNodeId getVal(const LoopSymState& st, il::ValueId v) const;
    brass::verify::BVNodeId toBool1(brass::verify::BVNodeId node);
    brass::verify::BVNodeId toBV64(brass::verify::BVNodeId node);
    brass::verify::BVNodeId toSInt32In64(brass::verify::BVNodeId node);
    LoopSymState applyEdge(const il::Function& fn, const LoopSymState& state, const il::BlockTarget& edge);
    bool analyzeHeader(const il::Function& fn, SingleLoopModel& out);
    bool evalToHeader(const il::Function& fn,
                      il::BlockId headerId,
                      il::BlockId cur,
                      LoopSymState state,
                      uint32_t steps,
                      std::vector<brass::verify::BVNodeId>& outLatchParams,
                      SymbolicHeapState* outHeap = nullptr);
    bool evalToRet(const il::Function& fn,
                   il::BlockId cur,
                   LoopSymState state,
                   uint32_t steps,
                   brass::verify::BVNodeId* outRet);
    bool execNonTerminators(const il::Function& fn, const il::Block& blk, LoopSymState& state);
};

// Proves equivalence of a single-loop function vs. a closed-form function (or
// two single-loop functions) over unbounded symbolic trip counts in
// `[domainMin, domainMax]` via k-induction over `brass::verify::BVContext`.
FunctionEquivResult compareIlLoopFunctionPairInductive(const il::Module& oldMod,
                                                       const il::Function& oldFn,
                                                       const il::Module& newMod,
                                                       const il::Function& newFn,
                                                       bool strictJs32Bitwise,
                                                       int32_t domainMin,
                                                       int32_t domainMax);

// Parses and lowers `loopSrc` and `closedFormSrc`, and runs inductive loop
// equivalence verification over `[domainMin, domainMax]`.
EquivReport proveLoopClosedFormInductive(std::string_view loopSrc,
                                         std::string_view closedFormSrc,
                                         int32_t domainMin,
                                         int32_t domainMax,
                                         std::string* errorOut);

}  // namespace bronze::check
