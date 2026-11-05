// The IL -> IL passes at the end of lowering, and the type-feedback
// specialization that reads a profile around them (opt/spec/feedback_plan.h).
//
// They run LAST: they read the finished module and hand out no module-global
// identity of their own, so everything lowering did before them — inline-cache
// indices, static-slot cells, template sites, the census table — is already
// frozen and a copied instruction shares the site the original named. It is
// also why `bronze il` shows the transformed IL: `lowerModule` is the single
// funnel both `runIl` and `runBuild` come through.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "lower/guard_region.h"
#include "lower/inline_guarded.h"
#include "lower/inline_small.h"
#include "lower/layout_facts.h"
#include "lower/loop_prop_analysis.h"
#include "lower/loop_prop_hoist.h"
#include "lower/lower_feedback.h"
#include "lower/lowerer.h"
#include "lower/own_callee_calls.h"
#include "lower/read_cse.h"
#include "lower/transition_runs.h"
#include "lower/typed_entry.h"
#include "lower/typed_loop_version.h"
#include "opt/spec/feedback_plan.h"
#include "il/print.h"
#include "opt/wpo/wpo.h"

namespace bronze::lower {

namespace {

// BRONZE_FINAL_IL_DUMP=<substr>[,...]: the final IL of every function whose
// name contains one of them, after the last pass, on stderr. A diagnostic.
void dumpFinalIl(const il::Module& mod) {
    const char* spec = std::getenv("BRONZE_FINAL_IL_DUMP");
    if (spec == nullptr || *spec == 0) return;
    const std::string all = spec;
    for (const il::Function& fn : mod.functions) {
        bool hit = false;
        for (size_t at = 0; at <= all.size() && !hit;) {
            const size_t comma = std::min(all.find(',', at), all.size());
            const std::string part = all.substr(at, comma - at);
            if (!part.empty() && fn.name.find(part) != std::string::npos) hit = true;
            at = comma + 1;
        }
        if (!hit) continue;
        il::Module one;
        one.keyConstants = mod.keyConstants;
        one.functions.push_back(fn);
        std::fputs(il::print(one).c_str(), stderr);
    }
}

}  // namespace

// With a `--feedback-in` profile the passes run twice. First on a copy of the
// module: its final IL is the recorded build's, whose keys the profile names,
// so that is where the profile is read (planFeedback). Then on the module,
// spending what the plan says at the points each claim has to be made.
bool Lowerer::finishIlPasses() {
    std::optional<spec::FeedbackPlan> plan;
    if (const spec::TypeProfile* profile = feedbackProfile()) {
        il::Module probe = ilModule_;
        DiagnosticSink probeDiags;
        if (!runFinalPasses(probe, probeDiags, nullptr, /*probe=*/true)) {
            for (const auto& d : probeDiags.all()) diags_.error(d.span, d.message);
            return false;
        }
        plan = spec::planFeedback(probe, *profile, ilModule_.functions.size());
    }
    return runFinalPasses(ilModule_, diags_, plan ? &*plan : nullptr, /*probe=*/false);
}

bool Lowerer::runFinalPasses(il::Module& mod, DiagnosticSink& diags, const spec::FeedbackPlan* plan, bool probe) {
    // The probe's whole-program solves report nothing: their census is the
    // real run's to print.
    const bool wholeProgram = wholeProgram_ != nullptr && wholeProgram_->on;
    wpo::BuildOptions build;
    if (wholeProgram) build = wholeProgram_->build;
    if (probe) build.stats = build.perSite = false;
    std::string scratch;
    std::string& report = wholeProgram && !probe ? wholeProgram_->report : scratch;
    spec::FeedbackApplyStats feedbackStats;

    // The predicted direct targets first, so that every pass below that
    // copies or specializes a callee reads them as it reads lowering's. A
    // profile's direct targets (B) right after, for the sites the prediction
    // left without one.
    OwnCallees ownCallees;
    if (wholeProgram) report += wpo::predictDirectTargets(mod, build, &ownCallees);
    if (plan != nullptr) {
        spec::applyFeedbackDirectMethods(mod, *plan, ownCallees, &feedbackStats);
        std::set<spec::SpanKey> offered;
        feedbackStats.closureCalls = applyClosureCalls(mod, [&](const il::Instruction& call) {
            const uint32_t target = spec::profiledClosureTarget(mod, *plan, call);
            if (target != UINT32_MAX) offered.insert(spec::spanKey(call.span));
            return target;
        });
        for (const spec::SpanKey& k : offered) feedbackStats.closureExecutions += plan->dynamicCallees.at(k).total;
    }
    inlineSmallFunctions(mod, &directMethods_);
    sanitizeLoopInBoundsProofs(mod);
    hoistLoopInvariantProps(mod, &directMethods_);
    eliminateRepeatedReads(mod);
    GuardRegionStats stats;
    applyGuardedRegions(mod, &stats);
    if (!probe) guardRegionStatsReport(stats);
    // After the regions, so that a region's fast copy, whose element
    // reads already stand behind `is.number`, is the loop it versions.
    ColdBlocks coldLoops;
    applyTypedLoopVersions(mod, &coldLoops);
    applyTypedEntries(mod, nullptr);
    // Last, so a body is copied as it is finished: its guarded regions
    // formed, a typed site's entry built and folded. Copied first, the
    // caller's regions could not form across it (they start at a
    // function's entry or a loop header, and an inlined body is neither),
    // and a typed site would lose its unboxed entry for a generic body.
    directMethods_.resolveAccessors(mod);
    if (inlineGuarded_) {
        std::vector<uint32_t> inlinedInto;
        const ClosureGuesses guesses = directMethods_.closureGuesses(mod);
        inlineGuardedMethods(mod, diags, &inlinedInto, &guesses, &coldLoops);
        if (diags.hasErrors()) return false;
        foldInlinedCallers(mod, inlinedInto);
    }
    // The own-slot calls inlining left: a direct entry behind the slot's
    // closure's code word.
    const uint32_t ownCalls = applyOwnCalleeCalls(mod, ownCallees);
    if (wholeProgram && build.stats) {
        report += "wpo-own: own-slot calls entered directly " + std::to_string(ownCalls) + "\n";
    }
    // A profile's guarded regions (D), on what inlining finished: the values
    // the run saw only as Numbers, carried as doubles behind one guard
    // (guard_region.h, "PROFILE-GUIDED REGIONS"). Before the passes that hand
    // out per-site cells (transition runs, define-add, the claims), so each
    // copy gets its own.
    if (plan != nullptr) {
        RegionFeedback regionFeedback;
        regionFeedback.operand = [plan](const il::Instruction& inst, uint32_t operand) {
            OperandFeedback out;
            const auto it = plan->values.find(spec::valueKey(inst, operand));
            if (it == plan->values.end()) return out;
            out.executions = it->second.total;
            if (it->second.numbers != it->second.total) {
                out.verdict = OperandFeedback::Other;
            } else if (it->second.total >= spec::kMinExecutions) {
                out.verdict = OperandFeedback::Number;
            }
            return out;
        };
        regionFeedback.entries = [plan](const il::Function& fn) -> uint64_t {
            const auto it = plan->entries.find(fn.name);
            return it == plan->entries.end() ? 0 : it->second;
        };
        GuardRegionStats feedbackRegions;
        applyGuardedRegions(mod, &feedbackRegions, &regionFeedback);
        guardRegionStatsReport(feedbackRegions, "guard-fb");
    }
    // After every pass that copies instructions: a copied run would share
    // its cells with the original (transition_runs.h).
    markTransitionRuns(mod);
    assignDefineAddCells(mod);
    // The claims last: they hand out static cells of their own, one per
    // site, so an inlined copy is claimed in its caller's context.
    if (wholeProgram) report += wpo::optimizeForBuild(mod, build);
    // A profile's claims (A) on the property sites the prediction left
    // unclaimed, and its number hints (C), before the layout facts spend
    // the claims.
    if (plan != nullptr) spec::applyFeedbackClaimsAndHints(mod, *plan, &feedbackStats);
    if (wholeProgram) {
        // After the claims, which it spends: a receiver's guard asked once
        // answers its later sites across calls (layout_facts.h).
        LayoutFactStats layoutStats;
        std::string layoutPerFn;
        applyLayoutFacts(mod, &layoutStats, build.perSite ? build.filter : std::string{}, &layoutPerFn);
        if (build.stats) report += layoutFactReport(layoutStats) + layoutPerFn;
    }
    if (plan != nullptr && wholeProgram && build.stats) report += spec::feedbackReport(*plan, feedbackStats);
    if (!probe) dumpFinalIl(mod);
    return true;
}

}  // namespace bronze::lower
