#pragma once

// THE OPTIMISTIC COPY S+ and TIERED RETIREMENT.
//
// A speculated function S has to be right for data unlike its harvest: an
// arm the harvested run never entered stays generic in S and rejoins its
// fast code, and that join costs S every value and shape it had loaded
// (the value walk keeps nothing across code that may call anything). Leaving
// there instead would keep them, but AOT code cannot re-profile: an arm cold
// in training and hot in use would leave on every call and retire S to G for
// good.
//
// So a function where an assumption pays only if its miss leaves gets TWO
// speculated copies of G:
//   - S, the CONSERVATIVE copy, exactly what the step made before: generic
//     arms rejoin;
//   - S+, the OPTIMISTIC copy (il.h Function::fallback names S): the same
//     rewrites, and where a miss would rejoin, it leaves instead. Its
//     assumptions:
//       * a block of G the harvested run never entered (never_entered.cpp)
//         leaves at its top;
//       * the generic arm of a polymorphic own-slot read or write (a chain or
//         a shape-set follower: the cache's ways were every receiver), and of
//         a store into a double slot (every store the run made was a Number),
//         leaves in front of the site (GuardBuilder::genericArm `mayLeave`).
//     Each such exit leaves for an OPTIMISTIC resume point of G (il_op.h
//     kResumeOptimistic): G's own point at the same place, under another id.
// S+ is entered first: every reference to G or S names S+ (driver.cpp).
// G's twin counts optimistic exits apart from the others: once a thread has
// left S+ through optimistic exits kOptimisticRetireAfter times
// (il2mir TranslatorOptions::deopt_optimistic_after), S+'s entry cell is
// pointed at S, and every call is S's from then on. S retires to G on its own
// exits as before, which S+'s ordinary guards (S has them too) also count
// toward. So data unlike the harvest costs at most that many leaves and then
// runs the code the step would have made without S+: no function ends up in
// G that would not have without it (docs/deopt-stats.md).
//
// An optimistic exit resumes in G, like every exit: the call it leaves
// finishes generically, and the next call enters S+ (or S once retired).
// Resuming in S instead would need a resume twin of S (brass builds twins of
// any function, but S's state at a point is in S's representations, which
// repr chose for S alone) and only matters for one long call, such as a loop
// that runs a whole program: there the rest of the loop runs in G, as it
// does after any leave today.
//
// INLINING. An optimistic caller inlines its callees' optimistic copies, a
// conservative caller their conservative ones (opt/inline driver.cpp), so S
// never holds an optimistic assumption: an inlined optimistic exit finishes
// the callee generically through an optimistic point of the callee's G and
// leaves the caller through an optimistic point of the caller's G after the
// call (made by the inliner beside the ordinary one). It counts toward
// retiring the caller's S+, and, through the resume.call at the callee's
// optimistic point, the callee's S+ too. A caller with no assumption of its own gets
// an S+ when it inlines a callee's (the inliner makes it).
//
// Only a function whose optimistic rewrite made an optimistic exit gets an
// S+; the rest keep one speculated copy.

#include <cstdint>
#include <vector>

#include "il/il.h"
#include "opt/speculate/context.h"
#include "opt/speculate/evidence.h"

namespace bronze::opt::speculate {

// The suffix of S+'s IL name after G's.
inline constexpr const char* kOptimisticSuffix = "$opt";

// The sites of `plan` outside the code an optimistic copy leaves before
// reaching: the blocks of G its never-entered blocks dominate.
std::vector<SitePlan> sitesOutsideLeftCode(const il::Function& generic, const FunctionPlan& plan);

// Turns each never-entered block of the optimistic copy (ctx.speculated,
// rewritten, by its id in G: a block a site split keeps its head under its
// own id) into a cold exit leaving for the optimistic point at its top, and
// deletes what no path reaches any more. Returns how many it turned.
uint32_t leaveNeverEntered(RewriteContext& ctx, const std::vector<BlockId>& blocks);

}  // namespace bronze::opt::speculate
