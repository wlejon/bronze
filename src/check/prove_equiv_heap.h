#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <verify/bv_solver.hpp>

#include "il/il.h"

namespace bronze::check {

inline constexpr size_t kMaxBoundedSeqLen = 8;

struct BoundedSeq {
    brass::verify::BVNodeId len = 0;
    brass::verify::BVNodeId elems[kMaxBoundedSeqLen] = {};
    bool isString = false;
};

// Shared table of canonical initial symbolic values for object fields and buffer
// elements read during symbolic IL execution (`this.x`, `arg_0.x`, `this.elements.0`,
// `arg_0[@idx]`, etc.). Sharing one table between `oldFn` and `newFn` ensures both
// functions read identical BVNodeIds for the same input property or buffer element.
class SymbolicHeapTable {
public:
    explicit SymbolicHeapTable(brass::verify::BVContext& ctx, bool floatMode = false)
        : ctx_(ctx), floatMode_(floatMode) {}

    brass::verify::BVContext& ctx() { return ctx_; }
    const brass::verify::BVContext& ctx() const { return ctx_; }

    bool isFloatMode() const { return floatMode_; }
    void setFloatMode(bool floatMode) { floatMode_ = floatMode; }

    int32_t domainMin() const { return domainMin_; }
    int32_t domainMax() const { return domainMax_; }
    void setDomainBounds(int32_t dMin, int32_t dMax) {
        domainMin_ = dMin;
        domainMax_ = dMax;
    }

    bool preferStringSeq() const { return preferStringSeq_; }
    void setPreferStringSeq(bool v) { preferStringSeq_ = v; }

    // Every parameter is a 32-bit integer (bro.solve, layout predicates). A
    // number turned into a string then has digits this model does not have,
    // so the conversion refuses instead of passing the number through.
    bool numericParams() const { return numericParams_; }
    void setNumericParams(bool v) { numericParams_ = v; }

    // Returns the canonical 64-bit sign-extended symbolic variable for `path`.
    brass::verify::BVNodeId getInitialVal(const std::string& path);

    // Returns a symbolic length variable for `path` (`base.length`) constrained to [0, 8].
    brass::verify::BVNodeId getBoundedLenVal(const std::string& path, bool isString);

    // Returns a symbolic character code variable for `path` (`base.k`) constrained to [0, 127].
    brass::verify::BVNodeId getCharCodeVal(const std::string& path);

    void markStringRoot(const std::string& basePath);
    void markArrayRoot(const std::string& basePath);
    bool isStringRoot(const std::string& basePath) const { return stringRoots_.count(basePath) > 0; }
    bool isArrayRoot(const std::string& basePath) const { return arrayRoots_.count(basePath) > 0; }
    bool hasBoundedLen(const std::string& basePath) const {
        return stringRoots_.count(basePath) > 0 || arrayRoots_.count(basePath) > 0 ||
               boundedLenPaths_.count(basePath + ".length") > 0;
    }
    bool isBoundedLenPath(const std::string& path) const { return boundedLenPaths_.count(path) > 0; }
    bool isCharCodePath(const std::string& path) const { return charCodePaths_.count(path) > 0; }
    bool usedBoundedStringLen() const { return usedBoundedStringLen_; }
    bool usedBoundedArrayLen() const { return usedBoundedArrayLen_; }

    brass::verify::BVNodeId buildBoundedSeqConstraintCond();

    // Returns a deterministic non-zero 64-bit identity token for an object reference
    // (`"this"`, `"arg_0"`, `"__local_0"`, etc.) so `return this;` matches across pairs.
    brass::verify::BVNodeId getObjectToken(const std::string& rootPath);

    void recordAliasedRoots(const std::string& rootA,
                            const std::string& rootB,
                            brass::verify::BVNodeId eqCond);

    // Builds a 1-bit conjunction enforcing `(eqCond => heap:rootA.k == heap:rootB.k)`
    // across all accessed suffixes `.k` for any pair of roots tested with `===`/`!==`,
    // or returns 0 if no alias comparisons were recorded.
    brass::verify::BVNodeId buildAliasCongruenceCond();

    const std::vector<std::string>& varPaths() const { return varPaths_; }
    const std::vector<brass::verify::BVNodeId>& varNodes() const { return varNodes_; }

private:
    struct AliasedRootPair {
        std::string rootA;
        std::string rootB;
        brass::verify::BVNodeId eqCond = 0;
    };
    brass::verify::BVContext& ctx_;
    bool floatMode_ = false;
    int32_t domainMin_ = INT32_MIN;
    int32_t domainMax_ = INT32_MAX;
    bool preferStringSeq_ = false;
    bool numericParams_ = false;
    bool usedBoundedStringLen_ = false;
    bool usedBoundedArrayLen_ = false;
    std::unordered_map<std::string, brass::verify::BVNodeId> initialVals_;
    std::unordered_map<std::string, brass::verify::BVNodeId> objectTokens_;
    std::vector<std::string> varPaths_;
    std::vector<brass::verify::BVNodeId> varNodes_;
    std::vector<AliasedRootPair> aliasedPairs_;
    std::unordered_set<std::string> stringRoots_;
    std::unordered_set<std::string> arrayRoots_;
    std::unordered_set<std::string> boundedLenPaths_;
    std::unordered_set<std::string> charCodePaths_;
};

struct PendingDivInfo {
    brass::verify::BVNodeId num = 0;
    brass::verify::BVNodeId den = 0;
    brass::verify::BVNodeId intOffset = 0;
};

struct ObjComparisonInfo {
    std::string lhsPath;
    std::string rhsPath;
    bool isEq = true;
};

// Per-path symbolic object/buffer state tracking object reference aliases and
// mutated property/element paths (`this.x`, `this.elements.0`, `arg_0[@i]`, etc.).
struct SymbolicHeapState {
    std::unordered_map<il::ValueId, std::string> objPaths;
    std::unordered_map<uint64_t, std::string> envObjPaths;
    std::unordered_map<std::string, std::string> propObjAliases;
    std::unordered_set<std::string> stringPaths;
    std::unordered_set<std::string> arrayPaths;
    std::unordered_map<il::ValueId, ObjComparisonInfo> objComparisons;
    std::unordered_map<il::ValueId, PendingDivInfo> divResults;
    std::unordered_map<brass::verify::BVNodeId, brass::verify::BVInterval> refinedIntervals;
    std::map<std::string, brass::verify::BVNodeId> writes;
    std::map<std::string, uint32_t> propFuncRefs;
    std::map<std::string, uint32_t> propFuncEnvs;
    std::map<std::string, std::string> propGlobalRefs;
    uint32_t localObjCounter = 0;

    std::string resolveObjPath(const std::string& p) const;
    bool isStringPath(const std::string& p) const;
    bool isStringValue(il::ValueId id) const;

    void initFunctionHeap(SymbolicHeapTable& table,
                          const il::Function& fn,
                          std::unordered_map<il::ValueId, brass::verify::BVNodeId>& valMap,
                          const std::string& thisPath = "this",
                          const std::vector<std::string>* callerArgPaths = nullptr);

    std::string lookupObjPath(il::ValueId id) const;

    void propagateAlias(il::ValueId src, il::ValueId dst);
    void propagateEnvSet(uint64_t envKey, il::ValueId src);
    void propagateEnvGet(uint64_t envKey, il::ValueId dst);

    void recordObjComparison(il::ValueId resultId, il::ValueId lhsId, il::ValueId rhsId, bool isEq);
    void unifyBranchAlias(SymbolicHeapTable& table,
                          il::ValueId condValId,
                          brass::verify::BVNodeId condBool1,
                          bool isTrueBranch);

    brass::verify::BVNodeId createLocalObject(SymbolicHeapTable& table, il::ValueId resultId);

    bool evalPropGet(SymbolicHeapTable& table,
                     il::ValueId recvId,
                     const std::string& key,
                     il::ValueId resultId,
                     brass::verify::BVNodeId& outVal,
                     bool* outIsUndefined = nullptr);

    bool evalPropSet(SymbolicHeapTable& table,
                     il::ValueId recvId,
                     const std::string& key,
                     brass::verify::BVNodeId valNode,
                     il::ValueId valId = il::kNoValue);

    bool evalElemGet(SymbolicHeapTable& table,
                     il::ValueId recvId,
                     brass::verify::BVNodeId idxNode,
                     il::ValueId resultId,
                     brass::verify::BVNodeId& outVal,
                     bool* outIsUndefined = nullptr);

    bool evalElemSet(SymbolicHeapTable& table,
                     il::ValueId recvId,
                     brass::verify::BVNodeId idxNode,
                     brass::verify::BVNodeId valNode,
                     il::ValueId valId = il::kNoValue);

    brass::verify::BVNodeId getFinalValue(SymbolicHeapTable& table,
                                          const std::string& path) const;

    static SymbolicHeapState mergeBranches(SymbolicHeapTable& table,
                                           brass::verify::BVNodeId condNode,
                                           const SymbolicHeapState& tHeap,
                                           const SymbolicHeapState& fHeap,
                                           brass::verify::BVNodeId tRet = 0,
                                           brass::verify::BVNodeId tThrew = 0,
                                           brass::verify::BVNodeId fRet = 0,
                                           brass::verify::BVNodeId fThrew = 0,
                                           brass::verify::BVNodeId mergedRet = 0);

    // Returns a 1-bit BVNodeId that is non-zero iff any non-local mutated property
    // or buffer element differs between `oldHeap` and `newHeap`, or 0 if identical.
    static brass::verify::BVNodeId buildHeapMismatchCond(SymbolicHeapTable& table,
                                                         const SymbolicHeapState& oldHeap,
                                                         const SymbolicHeapState& newHeap,
                                                         std::vector<std::string>* outCheckedPaths = nullptr);
};

int tryFoldRefinedBranchCond(
    const brass::verify::BVContext& ctx,
    brass::verify::BVNodeId condNode,
    int32_t dMin,
    int32_t dMax,
    const std::unordered_map<brass::verify::BVNodeId, brass::verify::BVInterval>& refined);

// Evaluates `Op::Div` or `Op::Mod` at `blk.instructions[instIdx]` with IEEE-754
// float-division awareness: only lowers `Op::Div` to integer division when
// consumed by integer truncation (`| 0`, `Math.trunc`, `Math.floor`, `Math.ceil`)
// or when the dividend is provably an exact multiple of the constant divisor.
bool evalDivOrModSymbolic(brass::verify::BVContext& ctx,
                          const il::Module& mod,
                          const il::Function& fn,
                          const il::Block& blk,
                          size_t instIdx,
                          brass::verify::BVNodeId a,
                          brass::verify::BVNodeId b,
                          SymbolicHeapState& heap,
                          brass::verify::BVNodeId& outVal,
                          std::string* outReason = nullptr,
                          int32_t domainMin = INT32_MIN,
                          int32_t domainMax = INT32_MAX);

// Evaluates `Math.floor` or `Math.ceil` on `argVal`, using exact integer
// floor/ceil division when `argId` was produced by `Op::Div(num, den)`.
brass::verify::BVNodeId evalFloorOrCeilSymbolic(brass::verify::BVContext& ctx,
                                                SymbolicHeapState& heap,
                                                il::ValueId argId,
                                                brass::verify::BVNodeId argVal,
                                                bool isCeil,
                                                int32_t domainMin = INT32_MIN,
                                                int32_t domainMax = INT32_MAX);

uint64_t hashStringLiteral(std::string_view s);

brass::verify::BVNodeId makeClz32Symbolic(brass::verify::BVContext& ctx,
                                          brass::verify::BVNodeId val64);

brass::verify::BVNodeId evalPowSymbolic(brass::verify::BVContext& ctx,
                                        brass::verify::BVNodeId base64,
                                        brass::verify::BVNodeId exp64,
                                        bool& ok,
                                        int32_t domainMin = INT32_MIN,
                                        int32_t domainMax = INT32_MAX);

const il::Function* findMethodCalleeInModule(const il::Module& mod,
                                             const SymbolicHeapState& heap,
                                             const std::string& recvPath,
                                             const std::string& method);

// Evaluates `Math.*`, `Number.*`, `String.*`, `Object.*`, and `Array.*` global
// method calls, returning true if handled (or if an explicit error was set in
// `*outErr`).
bool evalGlobalMethodCallSymbolic(brass::verify::BVContext& ctx,
                                  SymbolicHeapTable& heapTable,
                                  SymbolicHeapState& heap,
                                  const std::string& globalName,
                                  const std::string& method,
                                  const std::vector<il::ValueId>& operands,
                                  const std::vector<brass::verify::BVNodeId>& opVals,
                                  il::ValueId resultId,
                                  brass::verify::BVNodeId& outVal,
                                  std::vector<brass::verify::BVNodeId>* outArrayLiteral,
                                  std::string* outErr);

void minimizeCounterexampleModel(const brass::verify::BVContext& ctx,
                                 brass::verify::BVNodeId badCond,
                                 size_t totalVars,
                                 int32_t domainMin,
                                 int32_t domainMax,
                                 std::vector<uint64_t>& bestModel);

void minimizeFloatCounterexampleModel(const brass::verify::BVContext& ctx,
                                      brass::verify::BVNodeId badCond,
                                      size_t totalVars,
                                      std::vector<uint64_t>& bestModel);

uint32_t computeFunctionSymbolicLoopBound(const il::Function& fn,
                                          std::vector<uint8_t>& outInCycle);

bool functionRequiresFloatMode(const il::Module& mod, const il::Function& fn);
bool functionUsesStringOps(const il::Module& mod, const il::Function& fn);

uint64_t packConcreteString64(std::string_view s);
brass::verify::BVNodeId packBoundedStringNode(brass::verify::BVContext& ctx,
                                              brass::verify::BVNodeId len,
                                              const brass::verify::BVNodeId elems[kMaxBoundedSeqLen]);
brass::verify::BVNodeId selectBoundedSeqElem(brass::verify::BVContext& ctx,
                                             const BoundedSeq& seq,
                                             brass::verify::BVNodeId idx,
                                             brass::verify::BVNodeId defaultVal = 0);

brass::verify::BVNodeId makeF64ConstNode(brass::verify::BVContext& ctx, double d);

brass::verify::BVNodeId evalFloatToInt32Node(brass::verify::BVContext& ctx,
                                             brass::verify::BVNodeId f64);

brass::verify::BVNodeId roundSignedInt64AsDouble(brass::verify::BVContext& ctx,
                                                 brass::verify::BVNodeId val);

brass::verify::BVInterval getSignedInterval64(const brass::verify::BVContext& ctx,
                                              brass::verify::BVNodeId id,
                                              int32_t domainMin = INT32_MIN,
                                              int32_t domainMax = INT32_MAX,
                                              int depth = 16,
                                              const std::unordered_map<brass::verify::BVNodeId, brass::verify::BVInterval>* refined = nullptr);

brass::verify::BVNodeId evalFloatFloorSymbolic(brass::verify::BVContext& ctx,
                                               brass::verify::BVNodeId a);

brass::verify::BVNodeId evalFloatCeilSymbolic(brass::verify::BVContext& ctx,
                                              brass::verify::BVNodeId a);

brass::verify::BVNodeId evalFloatTruncSymbolic(brass::verify::BVContext& ctx,
                                               brass::verify::BVNodeId a);

brass::verify::BVNodeId evalFloatRoundSymbolic(brass::verify::BVContext& ctx,
                                               brass::verify::BVNodeId a);

brass::verify::BVNodeId evalFloatPowSymbolic(brass::verify::BVContext& ctx,
                                             brass::verify::BVNodeId base64,
                                             brass::verify::BVNodeId exp64,
                                             bool& ok);

bool evalFloatBinaryOpSymbolic(brass::verify::BVContext& ctx,
                               il::Op op,
                               brass::verify::BVNodeId lhs,
                               brass::verify::BVNodeId rhs,
                               brass::verify::BVNodeId& outVal,
                               std::string* outReason = nullptr);

bool tryEvalGlobalPropGetSymbolic(brass::verify::BVContext& ctx,
                                  const std::string& globalName,
                                  const std::string& key,
                                  bool floatMode,
                                  brass::verify::BVNodeId& outVal,
                                  bool& outIsMethodRef,
                                  std::string* outReason = nullptr);

}  // namespace bronze::check
