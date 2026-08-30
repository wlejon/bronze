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
    // What the affinity packer had to place: how many clusters the inlinable
    // edges left, and the biggest one. Both are diagnostics — a clustering that
    // collapsed the call graph shows up here as `clusters` near 1, and it is
    // the first thing to look at when a bin is not the size it should be.
    size_t clusters = 0;
    size_t largestCluster = 0;
    // What `BRONZE_XPART_PAD` actually added, which is nothing unless the pad
    // is at most a hundredth of the module — see partitionPadInsts.
    size_t padApplied = 0;
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

// Whether the legacy greedy-least-loaded packer is used instead of the
// affinity packer below. `BRONZE_XPART_LEGACY=1` is the A/B seam for the
// change; one binary answers both.
bool partitionUsesLegacyPacker();

// A measurement instrument, not a codegen knob: `BRONZE_XPART_PAD=<n>` adds n
// to the RECORDED size of the definition the packer places first — the biggest
// one, which on a bundle is the module top level — without touching a single
// instruction of anything.
//
// It exists because the packer's stability is otherwise unmeasurable. A real
// codegen change moves a function's size AND its code, so a benchmark delta
// after one cannot be attributed; padding moves only the number the packer
// reads, so every byte of every function is unchanged and any delta at all is
// the split's doing. Under the legacy packer a few hundred padded instructions
// re-seat a third of the module; that sweep is what the affinity packer is
// judged against.
//
// Applied only when it is at most a hundredth of the module: a pad larger than
// that is not a perturbation of a partition, it is a different module, and the
// answer it gives is about nothing. `PartitionPlan::padApplied` records what
// was really added, so a trace never claims a pad it did not take.
unsigned partitionPadInsts();

// The bin assignment as text, one `bin<TAB>insts<TAB>name` line per definition
// in name order, under a header naming the packer and the per-bin loads.
// `BRONZE_XPART_TRACE=1` writes it to stderr; two plans diff line-for-line, so
// "how many functions moved" is `diff | grep -c`. Name order, not bin order,
// precisely so that a function moving bins shows up as a changed line rather
// than as a shifted block.
std::string describePartition(const llvm::Module& m, const PartitionPlan& plan);

// Bin packing plus the keep sets.
//
// Definitions are first CLUSTERED by the inlinable-call edges below: a caller
// and a callee it asked to inline are unioned, so the pair lands in one bin and
// the split cannot come between them. Clusters — not functions — are then cut
// into bins by a running prefix sum over a size-ordered list, which is the
// stability property this exists for: the legacy greedy packer picks the
// least-loaded bin at every step, so one function growing by a few hundred
// instructions flips a comparison and cascades through every placement after
// it. A prefix cut cannot cascade. A size change shifts the running sum by that
// much and only a cluster sitting within that much of one of the `parts - 1`
// cut points can move, which bounds the damage at O(parts) clusters instead of
// O(functions).
//
// Deterministic in the only sense that matters here: the assignment is a pure
// function of the module — sizes with a name tie-break, clusters built in name
// order — with no hash-map iteration order and no timing anywhere in it, so
// the oracle's byte-for-byte objects hold.
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
