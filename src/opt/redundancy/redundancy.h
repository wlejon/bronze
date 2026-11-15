#pragma once

// THE REDUNDANCY PASS: the one optimiser pass that removes work a function
// does twice, does for nothing, or does on every iteration of a loop when once
// would do. It replaces the old optimiser's seven passes, each of which had a
// private idea of what clobbers what; this one has exactly one
// (location.h, `writeKillsRead`), read off il::effectsOf and refined only by
// an access's own operands and immediates.
//
// It runs on a speculated function S (guards are ordinary branches to cold
// blocks ending in `leave`) and on a generic function G (mostly
// `callsUnknown` operations, so it does little there, but it must be correct
// there, ResumePoints included). The jobs, cheapest first in each round:
//
//   1. value numbering of pure operations, constant folding and exact
//      identities (value_walk.cpp, fold.cpp);
//   2. folding of tests a branch edge already answered (tests.cpp), branches
//      on constants becoming jumps, unreachable blocks removed;
//   3. load elimination and store-to-load forwarding (value_walk.cpp over
//      expr_table.h), killed by the single invalidation rule;
//   5. dead store elimination (dead_stores.cpp);
//   6. dead code elimination (dead_code.cpp);
//   4. loop-invariant code motion, guards included (licm.cpp).
//
// Jobs 1-3 share one dominator-tree walk (dom_walk.h). The driver repeats the
// round until nothing changes or `maxRounds` is reached.
//
// The output is a function of the input IL alone: nothing iterates an
// unordered container in a way that decides a result.

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "il/il.h"

namespace bronze::opt {

// Where a guard hoisted out of a loop leaves to. See RedundancyOptions::
// preheaderResume for the contract.
struct LeaveTarget {
    // The `resume.point` id in G (S's `speculatedFrom`) the hoisted guard's
    // `leave` names.
    uint32_t resumeId = 0;
    // Builds the leave's state. Called once per hoisted guard, while the pass
    // applies its plan, with `exitBlock` a fresh, empty, cold block of `fn`
    // whose only predecessor is the hoisted guard's failing arm. It may
    // append exit-only instructions to that block (boxing, re-materialising)
    // with `opt::newValue`, but must not add blocks or edit any other block,
    // and returns the state in the resume point's operand order. The pass then
    // appends `leave @G, resumeId, state...` to the block.
    std::function<std::vector<il::ValueId>(il::Function& fn, il::BlockId exitBlock)> buildState;
};

// Answers, for the loop of `fn` whose header is `header` and whose preheader
// is `preheader`, where a guard executed at the end of the preheader can
// leave to.
//
// THE CONTRACT the speculation step implements:
//   - The answer is a resume point in G that corresponds to ENTERING this
//     loop: resumption there runs the loop from its first iteration with the
//     heap exactly as S has it at the end of `preheader`. Between the end of
//     `preheader` and the loop's first instruction S has only executed
//     instructions this pass hoisted, which neither write memory, throw,
//     allocate nor have observable order, so "the heap at loop entry" is
//     well defined.
//   - Every value the state uses must be available at the end of
//     `preheader` (defined there or in a block dominating it). Values
//     defined inside the loop are not.
//   - `header` and `preheader` are block ids of `fn` as it stands at the
//     call. The pass renumbers blocks between rounds (removing unreachable
//     ones), so ids recorded when S was made do not identify the loop;
//     identify it from the IL itself (for example the header's parameters
//     and the values the preheader's jump passes them).
//   - Returning nullopt keeps every guard of the loop inside it. Without a
//     resume point at the loop's entry a guard cannot be executed before the
//     loop: the per-iteration test remains, because the facts a guard
//     establishes do not survive the back edge into the next iteration
//     unless the guard dominates the header from outside.
using PreheaderResumeFn = std::function<std::optional<LeaveTarget>(
    const il::Function& fn, il::BlockId header, il::BlockId preheader)>;

struct RedundancyOptions {
    // Rounds of the job sequence per function; each round runs every job
    // once. Three is enough for the guard chains speculation makes
    // (is.object, then the shape test, then the loads it licenses).
    uint32_t maxRounds = 3;
    // Job 4. Off: nothing moves between blocks except by job 2's folding.
    bool hoistLoops = true;
    // See PreheaderResumeFn. Empty: no guard is hoisted (pure and read-only
    // instructions still are). Only consulted for a speculated function.
    PreheaderResumeFn preheaderResume;
    // Job 6 on a generic function G: delete each `resume.point` whose id is
    // not in `namedResumeIds` (sorted ascending). `runRedundancyModule` fills
    // the ids from the `leave`s of every speculated function; a direct caller
    // of `runRedundancy` that sets this must fill them itself. Off by default:
    // with nothing named, every resume point would go.
    bool removeUnnamedResumePoints = false;
    std::vector<uint32_t> namedResumeIds;
};

// What each job did, as counts. Accumulated, never reset by the pass.
struct RedundancyStats {
    uint64_t rounds = 0;
    // Job 1.
    uint64_t valuesNumbered = 0;     // a pure computation replaced by an equal earlier one
    uint64_t constantsFolded = 0;    // an operation on constants became a constant
    uint64_t identitiesFolded = 0;   // an operation replaced by one of its operands
    // Job 2.
    uint64_t testsFolded = 0;        // a test answered by the facts
    uint64_t branchesFolded = 0;     // a branch on a known condition became a jump
    uint64_t blocksRemoved = 0;      // unreachable blocks deleted (any job)
    // Job 3.
    uint64_t loadsEliminated = 0;    // a read replaced by an earlier read of the same location
    uint64_t loadsForwarded = 0;     // a read replaced by the value a store wrote
    // Job 4.
    uint64_t hoisted = 0;            // instructions moved to a preheader
    uint64_t guardsHoisted = 0;      // guards executed in a preheader instead
    // Job 5.
    uint64_t deadStores = 0;
    // Job 6.
    uint64_t deadInstructions = 0;
    uint64_t deadBlockParams = 0;
    uint64_t trivialBlockParams = 0; // a parameter every edge passes the same value
    uint64_t forwardersRemoved = 0;  // empty blocks that only jumped on
    uint64_t resumePointsRemoved = 0;

    void add(const RedundancyStats& o);
};

// Runs the pass on one function of `module`. Answers whether it changed
// anything. External functions and functions without blocks are left alone.
bool runRedundancy(il::Module& module, il::Function& fn, const RedundancyOptions& options,
                   RedundancyStats* stats = nullptr);

// Runs the pass on every function: speculated functions first, because the
// `leave`s that survive them decide which resume points of G are still named
// (`removeUnnamedResumePoints`, which this fills per G). Returns the totals.
RedundancyStats runRedundancyModule(il::Module& module, const RedundancyOptions& options);

}  // namespace bronze::opt
