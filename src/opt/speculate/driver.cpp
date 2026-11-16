// THE DRIVER: decides per function, clones, rewrites, places resume points,
// registers shape cells and redirects entry (speculate.h has the overview).
//
// Order matters, and is:
//   1. Read ALL evidence, for every function, on the module exactly as
//      lowering left it. The profile's keys hash each function's IL with the
//      callees it names (il/feedback_site.h functionIlHash), and the harvest's
//      identity is a digest of the IC layout: one resume point inserted, one
//      call redirected, and a key computed afterwards names nothing.
//   2. Fix every S's index before any rewrite, so a site that calls f can
//      name f's speculated entry whichever order the functions go in.
//   3. Per function: clone G into S, give S's lowered direct calls their
//      after-call points (markLoweredDirectCalls), rewrite S's sites last to
//      first in each block (a split at one site never moves another still to
//      come), place the loop-entry points. Then the same on a copy of G with
//      the optimistic rewrites (optimistic.h), never-entered blocks turned
//      into exits; it becomes S+ when it made an optimistic exit, appended
//      after the S copies, and is dropped otherwise. Then write G's resume
//      points (S's and S+'s together: they share a location's ordinary point).
//   4. Redirect entry over the whole module: to S+ where there is one (from G
//      and from S), else to S.
//
// Nothing here reads the whole-program analysis (opt/wpo wpo::analyze). It is
// the place a fact from it would enter a decision — a site the predicting
// solve already types needs no profile, and one it proves needs no guard —
// and the one hook for that is `planFromAnalysis` below, which answers
// nothing yet.

#include <algorithm>
#include <map>
#include <memory>

#include "il/resume_rules.h"
#include "opt/core/clone.h"
#include "opt/speculate/evidence.h"
#include "opt/speculate/optimistic.h"
#include "opt/speculate/policy.h"
#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

namespace {

// HOOK: whole-program facts (opt/wpo) that would add to or replace a profiled
// plan. Empty: the profile is the only evidence today.
void planFromAnalysis(const il::Module& module, FunctionPlan& plan) {
    (void)module;
    (void)plan;
}

void rewriteSite(RewriteContext& ctx, const SitePlan& site) {
    switch (site.kind) {
        case SiteKind::PropRead: rewritePropRead(ctx, site); return;
        case SiteKind::PropWrite: rewritePropWrite(ctx, site); return;
        case SiteKind::PropAdd: rewritePropAdd(ctx, site); return;
        case SiteKind::PropSlotRead:
        case SiteKind::PropSlotWrite: rewritePropSlot(ctx, site); return;
        case SiteKind::Construct: rewriteConstruct(ctx, site); return;
        case SiteKind::MethodSlot:
        case SiteKind::MethodProto: rewriteMethodCall(ctx, site); return;
        case SiteKind::Call: rewritePlainCall(ctx, site); return;
        case SiteKind::Arith: rewriteArith(ctx, site); return;
        case SiteKind::ElemRead:
        case SiteKind::ElemWrite: rewriteElem(ctx, site); return;
        case SiteKind::ArrayLength: rewriteArrayLength(ctx, site); return;
        case SiteKind::MathCall: rewriteMathCall(ctx, site); return;
        case SiteKind::MemberMethod:
        case SiteKind::MemberGet:
        case SiteKind::MemberSet:
        case SiteKind::MemberAbsent: rewriteMember(ctx, site); return;
        case SiteKind::TdzRead: rewriteTdzRead(ctx, site); return;
        case SiteKind::ArrayPush: rewriteArrayPush(ctx, site); return;
    }
}

// LOWERED DIRECT CALLS of S (lowering's own `call @f` to a declaration that is
// never rebound) get the same resume point after them that a call the
// rewrites make gets (direct_call.cpp): the point in G in front of the
// instruction after the call, recorded as the call's `resumeAfter`, so the
// inliner can take these too when f has guards of its own. Done on S as
// cloned, before any rewrite, while S's (block, index) are still G's; the
// field then travels with the instruction through every split. A point no
// inlined exit ends up naming is deleted by the redundancy pass with the
// other unnamed ones. Calls of declarations (runtime helpers, natives) and
// calls in blocks no point may go in are left without one.
void markLoweredDirectCalls(RewriteContext& ctx) {
    for (BlockId b = 0; b < ctx.speculated.blocks.size(); ++b) {
        if (!mayPlaceResumePoint(ctx.generic, b)) continue;
        std::vector<il::Instruction>& insts = ctx.speculated.blocks[b].instructions;
        for (uint32_t i = 0; i + 1 < insts.size(); ++i) {
            il::Instruction& inst = insts[i];
            if (inst.op != il::Op::Call || inst.resumeAfter != il::Instruction::kNoResumeAfter) continue;
            if (!il::canResumeBefore(ctx.generic.blocks[b], i + 1)) continue;
            if (inst.calleeIndex >= ctx.module.functions.size()) continue;
            const il::Function& callee = ctx.module.functions[inst.calleeIndex];
            if (callee.isExternal || callee.blocks.empty() || callee.isCoroutineBody()) continue;
            inst.resumeAfter = ctx.resume.at(b, i + 1);
        }
    }
}

// Sites in the order they are rewritten: block by block, last to first.
std::vector<SitePlan> rewriteOrder(std::vector<SitePlan> sites) {
    std::sort(sites.begin(), sites.end(), [](const SitePlan& a, const SitePlan& b) {
        if (a.block != b.block) return a.block < b.block;
        return a.index > b.index;
    });
    return sites;
}

}  // namespace

SpeculateStats speculateModule(il::Module& module, const spec::TypeProfile& profile,
                               const SpeculateOptions& options) {
    SpeculateStats stats;
    if (!options.enabled) return stats;
    // An instrumented build (`--feedback-out`) records; it never spends a
    // profile, and its `feedback.record`s would sit between a guard and its
    // site.
    if (!module.censusOutPath.empty()) return stats;

    // 1. Evidence, on the untouched module.
    const auto lowered = static_cast<uint32_t>(module.functions.size());
    std::vector<FunctionPlan> plans;
    {
        const Evidence evidence(module, profile, options, stats);
        // The keys the spelled harvest assumed eligible, registered at init.
        module.numberSlotKeys.clear();
        for (const std::string& key : evidence.numberKeys()) {
            const auto at = std::find(module.keyConstants.begin(), module.keyConstants.end(), key);
            module.numberSlotKeys.push_back(static_cast<uint32_t>(at - module.keyConstants.begin()));
        }
        stats.numberSlotKeys = static_cast<uint32_t>(module.numberSlotKeys.size());
        for (uint32_t f = 0; f < lowered; ++f) {
            ++stats.functionsConsidered;
            std::optional<FunctionPlan> plan = evidence.plan(f);
            if (!plan) continue;
            planFromAnalysis(module, *plan);
            if (plan->sites.empty()) continue;
            plan->neverEntered = evidence.neverEnteredBlocks(f);
            plans.push_back(std::move(*plan));
        }
    }
    if (plans.empty()) return stats;

    // 2. Every S's index: appended in plan order after the lowered functions.
    EntryMap entries(lowered);
    for (size_t i = 0; i < plans.size(); ++i) {
        entries.setSpeculated(plans[i].function, lowered + static_cast<uint32_t>(i));
    }

    // 3. Clone and rewrite, one function at a time. Each S+ is appended after
    // every S (step 2 fixed those indices).
    ShapeCells cells(module);
    std::vector<il::Function> kept;              // the S+ copies, appended after the loop
    std::map<uint32_t, uint32_t> optimisticOf;  // S -> S+
    for (const FunctionPlan& plan : plans) {
        const uint32_t g = plan.function;
        const std::string name = module.functions[g].name + options.nameSuffix;
        const uint32_t s = cloneFunction(module, g, name);
        assert(s == entries.entryOf(g) && "S lands where step 2 said");
        il::Function& speculated = module.functions[s];
        il::Function& generic = module.functions[g];
        speculated.speculatedFrom = g;

        ResumePoints resume(generic);
        RewriteContext ctx{module, g, s, generic, speculated, resume, cells, entries, valueTypes(speculated),
                           stats};
        markLoweredDirectCalls(ctx);
        for (const SitePlan& site : rewriteOrder(plan.sites)) rewriteSite(ctx, site);
        stats.loopEntryPoints += placeLoopEntries(ctx, plan.sites);

        // S+: G again, rewritten optimistically, kept when that made an
        // optimistic exit (optimistic.h).
        il::Function optimistic = generic;
        optimistic.name = module.functions[g].name + kOptimisticSuffix;
        optimistic.isExported = false;
        optimistic.isEntryPoint = false;
        optimistic.speculatedFrom = g;
        RewriteContext octx{module, g, s, generic, optimistic, resume, cells, entries, valueTypes(optimistic),
                            stats};
        octx.optimistic = true;
        markLoweredDirectCalls(octx);
        const std::vector<SitePlan> sites = sitesOutsideLeftCode(generic, plan);
        for (const SitePlan& site : rewriteOrder(sites)) rewriteSite(octx, site);
        stats.loopEntryPoints += placeLoopEntries(octx, sites, plan.neverEntered);
        const uint32_t arms = octx.optimisticExits;
        const uint32_t left = leaveNeverEntered(octx, plan.neverEntered);
        if (octx.optimisticExits != 0) {
            optimistic.fallback = s;
            kept.push_back(std::move(optimistic));
            ++stats.optimisticCopies;
            stats.optimisticArms += arms;
            stats.neverEnteredLeft += left;
        }

        stats.resumePoints += static_cast<uint32_t>(resume.size());
        resume.insertInto(generic);
        ++stats.functionsSpeculated;
    }
    stats.shapeCells = cells.added();
    for (il::Function& plus : kept) {
        const uint32_t sIndex = plus.fallback;
        module.functions.push_back(std::move(plus));
        optimisticOf[sIndex] = static_cast<uint32_t>(module.functions.size() - 1);
    }

    // 4. Entry goes to S+, or S.
    std::vector<uint32_t> entry(module.functions.size());
    for (uint32_t f = 0; f < entry.size(); ++f) entry[f] = entries.entryOf(f);
    for (const auto& [sIndex, plus] : optimisticOf) {
        entry[sIndex] = plus;
        entry[module.functions[sIndex].speculatedFrom] = plus;
    }
    stats.redirected = redirectFunctionReferences(module, entry);
    return stats;
}

}  // namespace bronze::opt::speculate
