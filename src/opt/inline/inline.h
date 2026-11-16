#pragma once

// THE INLINER: the one place a call is replaced by the body it calls.
//
// It runs once, on the speculated functions, after the speculation step
// (opt/speculate) and before the redundancy pass (opt/redundancy), and only
// ever replaces a DIRECT `call @F` in a speculated caller S_c. F is either a
// speculated function S_f, or a generic G_f that has no speculated copy (a
// direct call names a G only when it has none: the speculation step redirects
// every call of a speculated G to its S). The call becomes a jump into a copy
// of F's blocks (opt/core cloneBlocks: fresh value and block ids), F's
// parameters bound to the call's operands — which ARE F's parameter list,
// `__env` and `__this` included (the verifier's direct-call rule) — and each
// `ret v` of the copy a jump to the rest of the caller's block, whose
// parameter is the call's result.
//
// DEOPTIMISATION ACROSS AN INLINED CALL. A copy of S_f holds
// `leave @G_f, id, state...`, and a leave may only name its own function's G.
// So each one becomes, in its own exit block,
//
//     r = resume.call @G_f, id, state...     ; the rest of the CALLEE, generic
//     leave @G_c, a, callerState(r)          ; the rest of the CALLER, generic
//
// `resume.call` (il_op.h) runs G_f's resume twin from the point the guard
// stood for to G_f's return and yields its value; `a` is the resume point the
// speculation step placed in G_c right AFTER the generic call this direct
// call stands for (the call's `resumeAfter`, il.h), whose state is G_c's live
// values there. Exact: up to the failing guard the copy did what S_f would
// have done for this call, G_f from point `id` does the rest of exactly that
// call (that is what the leave in S_f already meant), and G_c from `a`
// continues the caller from just after the call with the call's value in the
// place of the generic call's result — for a speculated `new`, the value
// `construct.result` makes of it and the instance the call was given. Every other state value of `a` is
// S_c's own value of the same id (S keeps G's ids), defined before the call,
// so available in the copy, which the call's block dominates.
//
// BOTTOM-UP, ONE RULE. Callers are taken in post-order of the direct-call
// graph, so by the time S_f is inlined anywhere its own calls were inlined
// into it already, and its exits for them read
//     r = resume.call @G_g, ...;  leave @G_f, a', stateOfS_f(r)
// That last `leave @G_f` is a leave of S_f like any other: inlining S_f into
// S_c turns it, by the same rule, into
//     r2 = resume.call @G_f, a', stateOfS_f(r);  leave @G_c, a, callerState(r2)
// — the two-level finish: G_g finishes g's call, G_f from after that call
// finishes f's, G_c continues. Any depth follows the same way, so no chain
// of frames is kept anywhere: every leave of the body being inlined is
// rewritten by the one rule, and the `resume.call`s already there are copied
// as they are. A direct call inside the copy that was not inlined into F is an
// ordinary direct call and needs nothing; the copy's calls are never inlined
// again in this pass, which is also what bounds recursion (a self-call is
// inlined at most once: its copy's self-call stays a call).
//
// WHAT IS NOT INLINED (callee.cpp has the rules and why): a callee that is a
// declaration, the entry or a top-level function, a coroutine or starts one,
// reads new.target or calls super(), has an
// `arguments` object or a rest parameter, or whose returns do not match its
// return type; a call with an environment hop count above zero; a call whose
// callee has leaves but which has no `resumeAfter` (a protected block: the
// speculation step places no point there), or whose result's type in G_c
// cannot be made from G_f's return value.
//
// POLICY (driver.cpp): a callee of at most kSmallCallee instructions is
// inlined wherever the caller stays under kMaxCallerSize; one of at most
// kLargeCallee only within the caller's growth budget (kGrowthFactor times its
// size before the pass, at least kMinGrowth more). With a harvest's call
// counts (InlineOptions::harvestCount), a HOT call (a large enough share of
// all the calls counted) takes a callee of up to kHotCallee within a larger
// budget, and a call the harvested run never made takes only a small one.
// Sizes count instructions outside exit blocks (blocks ending in `leave`) of
// the callee once cleaned up (InlineOptions::cleanup). BRONZE_INLINE_TRACE=
// <file> appends one line per candidate and what was decided (a diagnostic).
//
// When done, the pass clears `resumeAfter` on every instruction of the module:
// it means nothing to anything after it, and the redundancy pass deletes the
// resume points nothing names any more (a `leave` or a `resume.call`).

#include <cstdint>
#include <functional>
#include <optional>

#include "il/il.h"

namespace bronze::opt::inliner {

struct InlineOptions {
    // Off: nothing is inlined, but `resumeAfter` is still cleared, so the
    // pipeline calls the pass either way.
    bool enabled = true;
    // The policy reads a callee's size off a copy this has cleaned up (the
    // redundancy pass), which is what the body costs once the caller is
    // cleaned up after the splice: a speculated body as made is mostly
    // guards the cleanup folds. Unset: the size as it stands.
    std::function<void(il::Function&)> cleanup;
    // The harvested run's count in slow-site word `index` (il/feedback_site.h:
    // a call's kCallCountArm word, a function's entry count), or nullopt when
    // the harvest has none for it (no harvest, a stale one, a word the
    // harvested build did not count). Unset: no counts at all.
    std::function<std::optional<uint64_t>(uint32_t index)> harvestCount;
};

struct InlineStats {
    uint32_t callersVisited = 0;      // speculated functions looked at
    uint32_t sitesConsidered = 0;     // direct calls in them
    uint32_t inlined = 0;             // calls replaced by a body
    uint32_t inlinedSpeculated = 0;   // of those, of a speculated callee
    uint32_t inlinedHot = 0;          // of those, taken as hot calls (above)
    uint32_t refusedCallee = 0;       // the callee can never be inlined
    uint32_t refusedSite = 0;         // this call cannot take it (env hops, no resume-after, types)
    uint32_t refusedSize = 0;         // too big for where the call is
    uint32_t refusedBudget = 0;       // the caller's growth budget was spent
    uint32_t leavesRewritten = 0;     // leave -> resume.call + leave
    uint32_t instructionsAdded = 0;   // copied instructions, exits included
};

// Runs the pass over `module` (see above). Deterministic: a function of the
// module alone.
InlineStats inlineModule(il::Module& module, const InlineOptions& options = {});

}  // namespace bronze::opt::inliner
