#pragma once

// THE SPECULATION STEP: the one place a profile changes code.
//
// Lowering emits every function as a GENERIC function G: complete, and it
// never speculates. For each function whose profile evidence is decisive and
// that is hot enough to matter, this step
//   1. clones G into a SPECULATED function S (opt/core cloneFunction;
//      `S.speculatedFrom` names G), keeping every value and block id;
//   2. rewrites sites of S into low-level operations behind GUARDS. A guard is
//      an ordinary `br`: its failing edge goes to a cold exit block ending in
//      `leave @G, id, state...`, and the fast code continues in a new block;
//   3. places, for every exit, a `resume.point id, state...` in G in front of
//      the generic instruction the guard stands for, so a failed guard
//      re-executes that one instruction in G and the rest of the call runs
//      generic; and a LOOP-ENTRY point at the top of every loop header of G
//      whose loop holds a guard (loopEntryResume below); and, for every
//      direct `call` of S (the call sites it speculated and lowering's own),
//      a point right AFTER the generic call, recorded as the call's
//      `resumeAfter` (il.h) for the inliner (opt/inline);
//   4. redirects entry: every creation of, reference to and direct call of G
//      in the module names S instead (S's own `leave`s keep naming G).
// There is no other speculation mechanism: no flags on instructions, no
// typed versions, no guarded regions. Everything S assumes it tests.
//
// What each site kind requires and emits is said in its own file
// (prop_read.cpp, prop_write.cpp, method_call.cpp, plain_call.cpp, arith.cpp,
// elem.cpp); evidence.cpp is the only reader of the profile.

#include <cstdint>
#include <optional>
#include <vector>

#include "il/il.h"
#include "opt/spec/profile.h"

namespace bronze::opt::speculate {

using il::BlockId;
using il::ValueId;

struct SpeculateOptions {
    // Off: nothing is speculated and the module is untouched.
    bool enabled = true;
    // A function's entries (a recorded profile) or samples (a harvest) below
    // these leave it generic; zero takes the defaults in evidence.h.
    uint64_t minEntries = 0;
    uint64_t minSamples = 0;
    // The suffix S's IL name gets after G's.
    const char* nameSuffix = "$spec";
};

struct SpeculateStats {
    uint32_t functionsConsidered = 0;
    uint32_t functionsSpeculated = 0;  // an S was made
    uint32_t harvestIgnored = 0;       // 1 when the file's harvest named another build
    uint32_t propReads = 0;
    uint32_t propReadChains = 0;       // of those, polymorphic
    uint32_t propWrites = 0;
    uint32_t methodCalls = 0;
    uint32_t plainCalls = 0;
    uint32_t arithmetic = 0;
    uint32_t elementReads = 0;
    uint32_t elementWrites = 0;
    uint32_t arrayLengths = 0;
    uint32_t mathCalls = 0;
    uint32_t guards = 0;               // branches whose failing edge leaves
    uint32_t resumePoints = 0;         // placed in generic functions
    uint32_t loopEntryPoints = 0;      // of those, at a loop header's top
    uint32_t shapeCells = 0;           // cells this step added to the module
    uint32_t redirected = 0;           // references to a G rewritten to its S
};

// Runs the step over `module`. Reads every piece of evidence before it
// changes anything: the profile's keys (il/feedback_site.h) name functions and
// sites of the module as lowering left it, and inline-cache and slow-site
// indices are never renumbered (S's generic leftovers keep G's, so the two
// share their caches).
SpeculateStats speculateModule(il::Module& module, const spec::TypeProfile& profile,
                               const SpeculateOptions& options = {});

// THE RESUME POLICY: whether a `resume.point` may be placed in `block` of the
// generic function `fn`. The backend decides what it can lower, and this is
// its rule (il/resume_rules.h canResumeIn / canResumeAt): no
// coroutine body or function starting one, not the module's top level, not a
// block a handler protects nor a handler block. Every placement in this step
// asks it, so a site in such a block simply stays generic in S.
bool mayPlaceResumePoint(const il::Function& fn, BlockId block);

// THE LOOP-ENTRY RESUME a guard hoisted out of a loop of S leaves to.
//
// Contract (the redundancy pass's PreheaderResumeFn): a guard executed at the
// end of `preheader`, before the loop headed by `header` runs its first
// iteration, fails. Resumption must run G's copy of the loop from its first
// iteration with the heap as S has it there. The speculation step placed, at
// the top of G's header H (in front of its first instruction, after its
// parameters are bound), a resume point whose state is `liveBefore(G, H, 0)`:
// H's parameters that are live, and the values defined before the loop that
// the loop or anything after it needs.
//
// The state a leave from the preheader hands over is that list with each of
// H's parameters replaced by the value the PREHEADER'S JUMP passes it (the
// parameter's value on loop entry) and every other value as S has it at the
// end of the preheader (S keeps G's value ids, so the id names S's copy).
// This answers that list, in the resume point's operand order:
//   - S's header is matched to G's H by the ids of its parameters, which are
//     G's (cloneFunction keeps ids; block renumbering by later passes does not
//     touch values). A header without parameters, or whose parameters name no
//     single block of G, answers nullopt.
//   - nullopt as well when a needed value no longer exists in S, is not
//     available at the end of `preheader` (defined inside the loop, or not on
//     every path), or has a type other than G's — or when `preheader` does not
//     end in a `jump` to `header`.
// `stateInS[i]` is to be passed for the resume point's operand i.
struct LoopEntryResume {
    uint32_t resumeId = 0;
    std::vector<ValueId> stateInS;
};
std::optional<LoopEntryResume> loopEntryResume(const il::Module& module, const il::Function& S,
                                               BlockId header, BlockId preheader);

}  // namespace bronze::opt::speculate
