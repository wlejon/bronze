#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llvm {
class Module;
}

namespace bronze::codegen_llvm {

// How a large module is split across emission threads, and — the part that is
// not just bin packing — which bodies each bin keeps that it does not OWN.
//
// The split assigns every definition to exactly one bin and every other bin
// turns it into a bare `declare`. That is correct and it is inlining-blind: a
// direct-call edge whose callee lands in another bin cannot be inlined, so a
// mechanism that spent a whole stage arranging for a call to be inlined loses
// it to a bin packer that has never heard of the edge. Stage E4 measured that
// happening to `Matrix4.multiplyMatrices` — 16.25 -> 18.09 ns/call with no
// change to either function, only to which bin each landed in.
//
// `keepBodies` is the repair: a direct-call callee under a size cap is kept in
// its caller's bin as `available_externally` — a definition the inliner may
// copy and the emitter must not emit, because the real one is emitted by the
// bin that owns it. Nothing is compiled twice and the partition objects stay
// disjoint, which the link would object to loudly if they did not.
struct PartitionPlan {
    // Definition name -> owning bin. Names, not pointers: each worker re-finds
    // its members in its own parsed copy of the module.
    std::unordered_map<std::string, unsigned> binOf;
    // Per bin, the names of definitions ANOTHER bin owns whose body this bin
    // keeps as `available_externally`.
    std::vector<std::unordered_set<std::string>> keepBodies;
};

// How many IR instructions an out-of-bin body may hold and still be kept.
// 0 disables the mechanism entirely (`BRONZE_NO_XPART_INLINE=1`), which is the
// A/B seam for it; `BRONZE_XPART_INLINE_CAP=<n>` overrides the shipped cap.
unsigned crossPartitionInlineCap();

// Whether every direct-call callee is a candidate, or only the callees of
// sites the compiler already marked `alwaysinline` — the direct method and
// closure edges stages 3.3 and E1 built, which is the set a blind split can
// silently un-inline. `BRONZE_XPART_INLINE_MODE=all` widens it; the shipped
// default is the narrow set, because the wide one measured the same on every
// fixture and cost a fifth again of the compile.
bool crossPartitionKeepsEveryDirectCall();

// How far a kept body's own direct calls are followed. 1 keeps the direct
// callees of the bin's members; 2 also keeps what those kept bodies call, and
// so on — the `a -> b -> c` split across three bins.
// `BRONZE_XPART_INLINE_DEPTH=<n>` overrides it.
unsigned crossPartitionInlineDepth();

// Greedy largest-first bin packing on instruction count, plus the keep sets.
// Deterministic: the sort's name tie-break fixes the assignment, and the keep
// sets are a fixpoint over it, so the same module always produces the same
// plan.
PartitionPlan planPartitions(const llvm::Module& m, unsigned parts);

// What one worker's partition cost, for the `--timings` report.
struct PartitionStats {
    size_t ownInsts = 0;
    size_t keptInsts = 0;
    size_t keptFns = 0;
};

// Reduce `part` — a freshly parsed copy of the whole module — to bin `bin`:
// materialize what this bin owns, keep what `plan` says to keep as
// `available_externally`, and delete the rest. Returns a non-empty string on
// failure. `part` may be lazily loaded; only the bodies this bin needs are
// materialized, which is what keeps peak memory at ~1x the module rather than
// Nx it.
std::string applyPartition(llvm::Module& part, const PartitionPlan& plan, unsigned bin,
                           PartitionStats& stats);

}  // namespace bronze::codegen_llvm
