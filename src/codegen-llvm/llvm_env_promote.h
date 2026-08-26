#pragma once

// STAGE R3: an environment slot becomes a register across a region no call can
// see. The observability question is `llvm_env_reach.h`; this is the region
// formation, the rewrite, and the write-back discipline.
//
// WHAT THE STAGE IS FOR. Stage E4 isolated the whole of `env_slot_kernel`'s
// remaining gap to node in one sentence: the record is heap-addressable and
// every call in the loop might reach it, so no slot is ever promoted across the
// backedge. The measurement was 29 loads and 47 stores per iteration against 3
// and 7 in a register-shaped probe of the same arithmetic
// (`bench/env_slot_kernel_registers.js`). Nothing about the arithmetic differs;
// what differs is that one form's state is in phis and the other's is in memory
// LLVM will not promote, because promoting it needs a fact only bronze holds.
//
// THE FACT BRONZE HOLDS. An environment record cannot be named by the accepted
// language — `with` is a parse error, `eval` runs with indirect semantics, no
// helper but the five `bronze_env_*` ABI entries touches a slot — so the set of
// operations that can observe a given slot is exactly: the accesses generated
// code emits for it, and the calls that can reach code that emits one. After
// the inliner has run, "the calls that can reach one" is a question about the
// module's own call graph, which `EnvReach` answers.
//
// THE REGION. A region is a span of code in which every operation that could
// observe the slot is one of the accesses the promotion rewrites. Two shapes
// are formed, and both are entered once and left once:
//
//   - A LOOP, entered through its preheader and left through its exit blocks.
//     This is the shape the stage exists for: N accesses per ITERATION become
//     one load per loop ENTRY and one store on the exit that is taken.
//   - A WHOLE FUNCTION whose body contains no observer at all, entered at the
//     record's definition and left at every `ret`. This is the leaf closure —
//     a getter, a small method — where the accesses are not in a loop.
//
// WRITE-BACK DISCIPLINE. Every edge that leaves a region carries the write-back,
// and the enumeration is short because of how bronze compiles control flow:
//
//   - normal exit, `break`, `continue` crossing the region boundary, and a
//     labelled jump are all EXIT EDGES of the natural loop, and an exit edge's
//     target is a dedicated exit block the write-back goes to the top of.
//   - `return` inside a loop cannot reach the latch, so it is not in the loop:
//     the edge to it is an exit edge like any other. In a function region a
//     `ret` is a write-back point directly.
//   - `throw` is a pending cell plus a RETURN, not an unwind. There is no
//     `invoke` in a bronze module and no landing pad; an exception leaves a
//     region through the same terminator an ordinary return does, and the
//     pending-flag branch that follows every call is an ordinary exit edge.
//   - `yield` and `await` are a return out of the generator body and a call
//     into a separately compiled `.resume` function, whose own entry reloads
//     the record. A suspension is not inside any region by construction.
//   - a block terminated by `unreachable` takes NO write-back. Those blocks are
//     the access guards' failure edges and the fatal helpers they call; the
//     process does not survive one, so there is no later reader to be given a
//     stale slot — and the pointer the write-back would store through is
//     precisely the one the guard just rejected.
//
// WHAT ENDS A REGION is enumerated by `RegionEnd` in `llvm_env_reach.h`, and
// every case is a refusal in the safe direction. There are no guards here and
// no optimism: where the call graph is incomplete — an escaped closure, a
// dynamic call, a call through a value — the region ends.
//
// HOW INLINING COMPOSES. The pass runs after the inliner, so a direct-edge
// callee that was inlined has its accesses IN the region, and they are rewritten
// with the caller's. A callee that was NOT inlined is a call, and ends the
// region. The nightmare case — the register live while inlined code writes the
// heap slot — cannot arise, because the thing that decides is the same IR both
// halves are read out of.
//
// THE COLLECTOR. A promoted register is invisible to the collector, and the heap
// slot the collector DOES scan goes stale the moment the register is written.
// So a key is promoted only when one of three holds: the region stores nothing
// into the slot (the heap stays exactly right), nothing in the region can
// collect, or every value stored into the slot in the region is provably not a
// heap pointer. The third is stage R2 speaking through `!bronze.env.nonptr`;
// the value the region STARTS with needs no argument, because until the first
// store the heap slot still holds it.
//
// THE ONE THING THAT CHANGES BESIDES SPEED. The entry load is speculative: it
// reads the slot on a path that might not have accessed it, and it reads it
// without the access guards. Both are the static plan's claim, the same one
// `BRONZE_ELIDE_ENV_GUARDS` rests on (llvm_env.cpp says what licenses it). What
// it costs is tripwire ORDER: if a lowering bug ever produced a record of the
// wrong kind, the promoted function faults at the entry load instead of
// reaching `bronze_env_access_failed`'s fatal. The guard itself is untouched
// and still armed at every access site that survives.

#include <cstdint>

#include <llvm/IR/PassManager.h>

#include "codegen-llvm/llvm_env_reach.h"

namespace llvm {
class Module;
}

namespace bronze::codegen_llvm {

// What the stage did to a module, as static counts. The counter exists for the
// reason `llvm_repr.h` gives for its own: every arm here is conditional on a
// proof, so a stage that proves nothing emits exactly the code it replaced and
// the whole suite still passes. The site count is the only thing separating
// "the fast arm is correct" from "the fast arm is ever taken".
struct EnvPromotionStats {
    uint32_t functions = 0;       // functions with at least one env-slot access
    uint32_t keys = 0;            // distinct (record, slot) pairs seen
    uint32_t loopRegions = 0;     // regions formed over a loop
    uint32_t functionRegions = 0; // regions formed over a whole function body
    uint32_t slotsPromoted = 0;   // (key, region) pairs promoted
    uint32_t loadsElided = 0;     // slot loads the region replaced
    uint32_t storesElided = 0;    // slot stores the region replaced
    uint32_t entryLoads = 0;      // loads added at region entry
    uint32_t writeBacks = 0;      // stores added at region exits
    uint32_t ends[static_cast<size_t>(RegionEnd::Count)] = {};
};

// The one process-global instance, bumped in place as modules are optimized.
EnvPromotionStats& envPromotionStats();

// Prints the counters when BRONZE_ENV_PROMOTION_STATS=1, including the
// region-end histogram — which is this stage's own tuning tool and the next
// stage's planning input.
void envPromotionStatsReport();

// BRONZE_NO_ENV_PROMOTION=1: the analysis does not run and the emitted code is
// identical to what the stage-R2 compiler produced. Read by the COMPILER, once
// per invocation, because what it isolates is the emitted code — a run-time
// flag could not change it.
bool envPromotionDisabled();

// The pass. Added at `registerOptimizerEarlyEPCallback`, which is after the
// module simplification pipeline — inlining, SROA, EarlyCSE, GVN, LoopSimplify,
// LICM — and before the function optimization pipeline, so the phis it creates
// are there for everything downstream to read. `out`, when non-null, also
// receives this run's counts, which is how the unit tests read them without
// touching the global.
class EnvPromotionPass : public llvm::PassInfoMixin<EnvPromotionPass> {
public:
    explicit EnvPromotionPass(EnvPromotionStats* out) : out_(out) {}
    EnvPromotionPass() : out_(nullptr) {}

    llvm::PreservedAnalyses run(llvm::Module& llvmModule, llvm::ModuleAnalysisManager& mam);

private:
    EnvPromotionStats* out_;
};

}  // namespace bronze::codegen_llvm
