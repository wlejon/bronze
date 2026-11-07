#pragma once

#include <cstdint>
#include <map>
#include <string>
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

    const std::vector<std::string>& varPaths() const { return varPaths_; }
    const std::vector<brass::verify::BVNodeId>& varNodes() const { return varNodes_; }

private:
    brass::verify::BVContext& ctx_;
    std::unordered_map<std::string, brass::verify::BVNodeId> initialVals_;
    std::unordered_map<std::string, brass::verify::BVNodeId> objectTokens_;
    std::vector<std::string> varPaths_;
    std::vector<brass::verify::BVNodeId> varNodes_;
};

// Per-path symbolic object/buffer state tracking object reference aliases and
// mutated property/element paths (`this.x`, `this.elements.0`, `arg_0[@i]`, etc.).
struct SymbolicHeapState {
    std::unordered_map<il::ValueId, std::string> objPaths;
    std::unordered_map<uint64_t, std::string> envObjPaths;
    std::map<std::string, brass::verify::BVNodeId> writes;
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

    brass::verify::BVNodeId createLocalObject(SymbolicHeapTable& table, il::ValueId resultId);

    bool evalPropGet(SymbolicHeapTable& table,
                     il::ValueId recvId,
                     const std::string& key,
                     il::ValueId resultId,
                     brass::verify::BVNodeId& outVal);

    bool evalPropSet(SymbolicHeapTable& table,
                     il::ValueId recvId,
                     const std::string& key,
                     brass::verify::BVNodeId valNode,
                     il::ValueId valId = il::kNoValue);

    bool evalElemGet(SymbolicHeapTable& table,
                     il::ValueId recvId,
                     brass::verify::BVNodeId idxNode,
                     il::ValueId resultId,
                     brass::verify::BVNodeId& outVal);

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

}  // namespace bronze::check
