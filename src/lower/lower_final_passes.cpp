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
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "lower/function_placement.h"
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
#include "lower/truth_join_threading.h"
#include "lower/typed_entry.h"
#include "lower/typed_body_version.h"
#include "lower/typed_loop_version.h"
#include "opt/spec/feedback_plan.h"
#include "il/feedback_site.h"
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
    // After the lowering's own families (lowering is over, and their ids are
    // in its claims) and before any claim of this run.
    if (plan != nullptr) spec::addProfiledFamilies(mod, *plan);

    // The predicted direct targets first, so that every pass below that
    // copies or specializes a callee reads them as it reads lowering's. A
    // profile's direct targets (B) right after, for the sites the prediction
    // left without one.
    OwnCallees ownCallees;
    OwnCallees predictedAccessors;
    ClosureGuesses polyMethods;
    ClosureGuesses polyOwnCallees;
    if (wholeProgram) report += wpo::predictDirectTargets(mod, build, &ownCallees, &predictedAccessors);
    if (plan != nullptr) {
        spec::applyFeedbackDirectMethods(mod, *plan, ownCallees, &feedbackStats, &polyMethods, &polyOwnCallees);
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
    // The loop-free bodies the loop pass leaves, over views they are handed.
    applyTypedBodyVersions(mod, &coldLoops);
    applyTypedEntries(mod, nullptr);
    // Last, so a body is copied as it is finished: its guarded regions
    // formed, a typed site's entry built and folded. Copied first, the
    // caller's regions could not form across it (they start at a
    // function's entry or a loop header, and an inlined body is neither),
    // and a typed site would lose its unboxed entry for a generic body.
    directMethods_.resolveAccessors(mod);
    if (!predictedAccessors.empty()) directMethods_.applyPredictedAccessors(mod, predictedAccessors);
    // A profile's accessor targets (F), for the sites the class guess left
    // without one, where that guess is stamped: right before the inliner,
    // which copies a getter's or setter's body in behind its cell.
    if (plan != nullptr) {
        spec::applyFeedbackAccessors(
            mod, *plan,
            [&](const std::string& proto, const std::string& key, bool setter) {
                return directMethods_.accessorOfShownClass(mod, proto, key, setter);
            },
            &feedbackStats);
    }
    std::unordered_set<uint32_t> inlinedOwnIcs;
    if (inlineGuarded_) {
        std::vector<uint32_t> inlinedInto;
        ClosureGuesses guesses = directMethods_.closureGuesses(mod);
        for (const auto& [ic, target] : ownCallees) guesses[ic] = {target};
        for (const auto& [ic, targets] : polyMethods) {
            (void)targets;
            guesses.erase(ic);
        }
        for (const auto& [ic, targets] : polyOwnCallees) {
            (void)targets;
            guesses.erase(ic);
        }
        inlineGuardedMethods(mod, diags, &inlinedInto, &guesses, &coldLoops,
                             polyMethods.empty() ? nullptr : &polyMethods,
                             polyOwnCallees.empty() ? nullptr : &polyOwnCallees, &inlinedOwnIcs);
        if (diags.hasErrors()) return false;
        // An inlined method body's guard gets a cell pair of its own
        // (runtime/accessor_cell.cpp, `bronze_method_cell_publish`): one per
        // site and guessed callee, shared by every copy of the guard. Cell 0
        // is "none", so it is never handed out here.
        {
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> cellOf;
            for (uint32_t f : inlinedInto) {
                for (il::Block& blk : mod.functions[f].blocks) {
                    for (il::Instruction& inst : blk.instructions) {
                        if (inst.op != il::Op::MethodIcIs || inst.staticCellIndex != 0) continue;
                        auto cell = cellOf.find({inst.icIndex, inst.calleeIndex});
                        if (cell == cellOf.end()) {
                            if (mod.staticSiteCount == 0) mod.staticSiteCount = 1;
                            cell = cellOf.emplace(std::pair{inst.icIndex, inst.calleeIndex}, mod.staticSiteCount).first;
                            mod.staticSiteCount += 2;
                        }
                        inst.staticCellIndex = cell->second;
                    }
                }
            }
        }
        foldInlinedCallers(mod, inlinedInto);
        // The loops inlined bodies brought (guard_region.h, `only`).
        GuardRegionStats inlinedRegions;
        applyGuardedRegions(mod, &inlinedRegions, nullptr, &inlinedInto);
        if (!probe) guardRegionStatsReport(inlinedRegions, "guard-inl");
    }
    for (uint32_t ic : inlinedOwnIcs) ownCallees.erase(ic);
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
    // The exits the typed bodies left in front of their reads, now that every
    // read that will have a claim has it.
    resolveReadExits(mod);
    if (wholeProgram) {
        // After the claims, which it spends: a receiver's guard asked once
        // answers its later sites across calls (layout_facts.h).
        LayoutFactStats layoutStats;
        std::string layoutPerFn;
        applyLayoutFacts(mod, &layoutStats, build.perSite ? build.filter : std::string{}, &layoutPerFn);
        if (build.stats) report += layoutFactReport(layoutStats) + layoutPerFn;
    }
    if (plan != nullptr && wholeProgram && build.stats) report += spec::feedbackReport(*plan, feedbackStats);
    // The harvest identity (il.h icLayoutDigest), before the threading below,
    // which the probe does not run: a plain build's digest is the one a later
    // build's probe computes here. A build that spent a profile says so, and
    // its caches are not harvested (docs/type-feedback.md, "Harvest").
    mod.icLayoutDigest = il::icLayoutDigest(mod);
    if (plan != nullptr && !probe) {
        mod.feedbackApplied = plan->stats.matchedFunctions != 0 || plan->stats.harvestSites != 0;
    }
    // Last: control flow only, after every pass that reads or copies a site.
    // Not in the probe, nor in a build a profile is recorded from: a profile
    // numbers its sites in the order the final IL has them, and the probe is
    // read against it as the recorded build had it, so both stay unthreaded.
    const bool recording = feedback_ != nullptr && !feedback_->outPath.empty();
    if (!probe && !recording) threadTruthJoins(mod);
    // Where each function goes in the text (function_placement.h): from the
    // profile when there is one, last of all, as the module now stands.
    if (!probe) {
        const PlacementStats placement = planFunctionPlacement(mod, plan);
        if (wholeProgram && build.stats) report += placementReport(placement);
    }
    if (!probe) dumpFinalIl(mod);
    return true;
}

}  // namespace bronze::lower
