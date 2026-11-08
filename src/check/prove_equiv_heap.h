#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <verify/bv_solver.hpp>

#include "il/il.h"

namespace bronze::check {

// Shared table of canonical initial symbolic values for object fields and buffer
// elements read during symbolic IL execution (`this.x`, `arg_0.x`, `this.elements.0`,
// `arg_0[@idx]`, etc.). Sharing one table between `oldFn` and `newFn` ensures both
// functions read identical BVNodeIds for the same input property or buffer element.
class SymbolicHeapTable {
public:
    explicit SymbolicHeapTable(brass::verify::BVContext& ctx) : ctx_(ctx) {}

    brass::verify::BVContext& ctx() { return ctx_; }
    const brass::verify::BVContext& ctx() const { return ctx_; }

    // Returns the canonical 64-bit sign-extended symbolic variable for `path`.
    brass::verify::BVNodeId getInitialVal(const std::string& path);

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
    std::unordered_map<std::string, brass::verify::BVNodeId> initialVals_;
    std::unordered_map<std::string, brass::verify::BVNodeId> objectTokens_;
    std::vector<std::string> varPaths_;
    std::vector<brass::verify::BVNodeId> varNodes_;
    std::vector<AliasedRootPair> aliasedPairs_;
};

struct PendingDivInfo {
    brass::verify::BVNodeId num = 0;
    brass::verify::BVNodeId den = 0;
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
    std::unordered_map<il::ValueId, ObjComparisonInfo> objComparisons;
    std::unordered_map<il::ValueId, PendingDivInfo> divResults;
    std::map<std::string, brass::verify::BVNodeId> writes;
    std::map<std::string, uint32_t> propFuncRefs;
    uint32_t localObjCounter = 0;

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
                                           const SymbolicHeapState& fHeap);

    // Returns a 1-bit BVNodeId that is non-zero iff any non-local mutated property
    // or buffer element differs between `oldHeap` and `newHeap`, or 0 if identical.
    static brass::verify::BVNodeId buildHeapMismatchCond(SymbolicHeapTable& table,
                                                         const SymbolicHeapState& oldHeap,
                                                         const SymbolicHeapState& newHeap,
                                                         std::vector<std::string>* outCheckedPaths = nullptr);
};

// Evaluates `Op::Div` or `Op::Mod` at `blk.instructions[instIdx]` with IEEE-754
// float-division awareness: only lowers `Op::Div` to integer division when
// consumed by integer truncation (`| 0`, `Math.trunc`, `Math.floor`, `Math.ceil`)
// or when the dividend is provably an exact multiple of the constant divisor.
bool evalDivOrModSymbolic(brass::verify::BVContext& ctx,
                          const il::Module& mod,
                          const il::Block& blk,
                          size_t instIdx,
                          brass::verify::BVNodeId a,
                          brass::verify::BVNodeId b,
                          SymbolicHeapState& heap,
                          brass::verify::BVNodeId& outVal,
                          std::string* outReason = nullptr);

// Evaluates `Math.floor` or `Math.ceil` on `argVal`, using exact integer
// floor/ceil division when `argId` was produced by `Op::Div(num, den)`.
brass::verify::BVNodeId evalFloorOrCeilSymbolic(brass::verify::BVContext& ctx,
                                                const SymbolicHeapState& heap,
                                                il::ValueId argId,
                                                brass::verify::BVNodeId argVal,
                                                bool isCeil);

uint64_t hashStringLiteral(std::string_view s);

brass::verify::BVNodeId makeClz32Symbolic(brass::verify::BVContext& ctx,
                                          brass::verify::BVNodeId val64);

brass::verify::BVNodeId evalPowSymbolic(brass::verify::BVContext& ctx,
                                        brass::verify::BVNodeId base64,
                                        brass::verify::BVNodeId exp64,
                                        bool& ok);

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

}  // namespace bronze::check
