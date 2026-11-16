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
//      come), place the loop-entry points, then write G's resume points.
//   4. Redirect entry over the whole module.
//
// Nothing here reads the whole-program analysis (opt/wpo wpo::analyze). It is
// the place a fact from it would enter a decision — a site the predicting
// solve already types needs no profile, and one it proves needs no guard —
// and the one hook for that is `planFromAnalysis` below, which answers
// nothing yet.

#include <algorithm>
#include <memory>

#include "il/resume_rules.h"
#include "opt/core/clone.h"
#include "opt/speculate/evidence.h"
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
        case SiteKind::MethodSlot:
        case SiteKind::MethodProto: rewriteMethodCall(ctx, site); return;
        case SiteKind::Call: rewritePlainCall(ctx, site); return;
        case SiteKind::Arith: rewriteArith(ctx, site); return;
        case SiteKind::ElemRead:
        case SiteKind::ElemWrite: rewriteElem(ctx, site); return;
    }
}

// The operations that name a function by index, and whether each names the
// function's CODE (and so must follow entry to S) — `leave` names G on
// purpose and is not among them; `coro.start` names a coroutine body, which
// is never speculated.
uint32_t* redirectableIndex(il::Instruction& inst) {
    switch (inst.op) {
        case il::Op::Call:
        case il::Op::CreateFunction:
        case il::Op::FunctionRef:
        case il::Op::FnCodeIs:
            return &inst.calleeIndex;
        case il::Op::Construct:
            return inst.directTarget != il::Instruction::kNoDirectTarget ? &inst.directTarget : nullptr;
        default:
            return nullptr;
    }
}

// Every creation of, reference to and direct call of a speculated G now
// names its S: the closures a program makes run S, and a direct call enters
// S. That includes G's own calls of G (a recursion in G reached by a leave
// re-enters the speculated code) and S's. The `leave`s of S keep naming G.
uint32_t redirectEntry(il::Module& module, const EntryMap& entries) {
    uint32_t redirected = 0;
    for (il::Function& fn : module.functions) {
        for (il::Block& block : fn.blocks) {
            for (il::Instruction& inst : block.instructions) {
                uint32_t* index = redirectableIndex(inst);
                if (index == nullptr || !entries.isRedirected(*index)) continue;
                *index = entries.entryOf(*index);
                ++redirected;
            }
        }
    }
    return redirected;
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
        for (uint32_t f = 0; f < lowered; ++f) {
            ++stats.functionsConsidered;
            std::optional<FunctionPlan> plan = evidence.plan(f);
            if (!plan) continue;
            planFromAnalysis(module, *plan);
            if (!plan->sites.empty()) plans.push_back(std::move(*plan));
        }
    }
    if (plans.empty()) return stats;

    // 2. Every S's index: appended in plan order after the lowered functions.
    EntryMap entries(lowered);
    for (size_t i = 0; i < plans.size(); ++i) {
        entries.setSpeculated(plans[i].function, lowered + static_cast<uint32_t>(i));
    }

    // 3. Clone and rewrite, one function at a time.
    ShapeCells cells(module);
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
        stats.resumePoints += static_cast<uint32_t>(resume.size());
        resume.insertInto(generic);
        ++stats.functionsSpeculated;
    }
    stats.shapeCells = cells.added();

    // 4. Entry goes to S.
    stats.redirected = redirectEntry(module, entries);
    return stats;
}

}  // namespace bronze::opt::speculate
