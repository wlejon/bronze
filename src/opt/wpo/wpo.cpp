#include "opt/wpo/wpo.h"

#include <chrono>
#include <map>
#include <set>
#include <tuple>

#include "opt/wpo/census.h"
#include "opt/wpo/shape.h"
#include "opt/wpo/solver.h"
#include "opt/wpo/stamp.h"

namespace bronze::wpo {

namespace {

// A prediction keeps the escaped objects a proof folds into the unknown, so
// its sets are as large as the program's whole escaped heap where a proof's
// are one unknown. A set past this many objects predicts no slot a guard could
// use anyway, and widening it keeps the solve as fast as the proof's.
constexpr size_t kPredictSetCap = 128;

SolverOptions solverOptions(const BuildOptions& opts, bool predict) {
    SolverOptions o;
    o.hostGlobals = opts.hostGlobals;
    o.predict = predict;
    if (predict) o.setCap = kPredictSetCap;
    return o;
}

double millisSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::string census(const il::Module& mod, const BuildOptions& opts, bool predict, const StampPlan* plan) {
    Solver solver(mod, solverOptions(opts, predict));
    solver.run();
    const auto start = std::chrono::steady_clock::now();
    ShapeAnalysis shapes(solver);
    shapes.run();
    const double shapeMillis = millisSince(start);
    auto facts = classifySites(solver, shapes);
    if (plan != nullptr) {
        std::map<std::tuple<uint32_t, uint32_t, uint32_t>, const StampEntry*> byLoc;
        for (const StampEntry& e : plan->entries) byLoc[{e.fn, e.block, e.inst}] = &e;
        std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> directAt;
        for (const DirectEntry& d : plan->directs) directAt[{d.fn, d.block, d.inst}] = d.target;
        std::set<std::tuple<uint32_t, uint32_t, uint32_t>> polyAt;
        for (const PolyEntry& p : plan->polys) polyAt.insert({p.fn, p.block, p.inst});
        std::set<std::tuple<uint32_t, uint32_t, uint32_t>> absentAt;
        for (const PolyEntry& p : plan->absents) absentAt.insert({p.fn, p.block, p.inst});
        for (SiteFact& f : facts) {
            if (auto d = directAt.find({f.fn, f.block, f.inst}); d != directAt.end()) {
                f.wpoStamp = "direct " + mod.functions[d->second].name;
                continue;
            }
            auto it = byLoc.find({f.fn, f.block, f.inst});
            if (it == byLoc.end()) {
                if (polyAt.count({f.fn, f.block, f.inst}) != 0) f.wpoStamp = "poly";
                if (absentAt.count({f.fn, f.block, f.inst}) != 0) f.wpoStamp = "absent";
                continue;
            }
            f.wpoStamp = it->second->form == StampForm::Identity
                             ? "identity"
                             : "family " + std::to_string(it->second->familyLo) + "+" +
                                   std::to_string(it->second->familySpan);
        }
    }
    CensusOptions censusOpts;
    censusOpts.perSite = opts.perSite;
    censusOpts.filter = opts.filter;
    censusOpts.prefix = predict ? "wpo-pred" : "wpo";
    return formatCensus(solver, shapes, facts, censusOpts, shapeMillis);
}

// Whether `name` contains one of the comma-separated substrings of `filter`.
bool matchesFilter(const std::string& name, const std::string& filter) {
    for (size_t at = 0; at <= filter.size();) {
        const size_t comma = std::min(filter.find(',', at), filter.size());
        if (name.find(filter.substr(at, comma - at)) != std::string::npos) return true;
        at = comma + 1;
    }
    return false;
}

std::string stampReport(const il::Module& mod, const StampPlan& plan, const BuildOptions& opts) {
    uint32_t identity = 0, family = 0;
    struct Row {
        uint32_t identity = 0, family = 0, direct = 0, poly = 0;
    };
    std::map<uint32_t, Row> perFn;
    for (const DirectEntry& d : plan.directs) ++perFn[d.fn].direct;
    for (const PolyEntry& p : plan.polys) ++perFn[p.fn].poly;
    for (const StampEntry& e : plan.entries) {
        auto& row = perFn[e.fn];
        if (e.form == StampForm::Identity) {
            ++identity;
            ++row.identity;
        } else {
            ++family;
            ++row.family;
        }
    }
    std::string out = "wpo-stamp: predicted static slots " + std::to_string(plan.entries.size()) + " of " +
                      std::to_string(plan.candidates) + " candidate sites: identity " + std::to_string(identity) +
                      " (" + std::to_string(plan.identityInexact) + " of one site whose shape is inexact)" +
                      ", family " + std::to_string(family) + "; not stamped: no class families " +
                      std::to_string(plan.noFamilies) + ", a receiver no family describes " +
                      std::to_string(plan.noStamp) + ", families disagree on the slot " +
                      std::to_string(plan.rangeMismatch) + "; module families " +
                      std::to_string(mod.classFamilies.size()) + "\n";
    out += "wpo-stamp: predicted direct method targets " + std::to_string(plan.directs.size()) + " of " +
           std::to_string(plan.callCandidates) + " single-callee sites without one (lowering's guess withdrawn at " +
           std::to_string(plan.callWithdrawn) + "); refused: the callee may sit in an own slot " +
           std::to_string(plan.callOwnHeld) + ", the callee's parameters " + std::to_string(plan.callRefused) + "\n";
    out += "wpo-stamp: predicted polymorphic reads " + std::to_string(plan.polys.size()) + " of " +
           std::to_string(plan.polyCandidates) + " own-data reads without a claim; more receivers than cache ways " +
           std::to_string(plan.polyTooMany) + "\n";
    out += "wpo-stamp: predicted absent reads " + std::to_string(plan.absents.size()) + "\n";
    if (!opts.perSite) return out;
    for (const auto& [fn, row] : perFn) {
        const std::string& name = mod.functions[fn].name;
        if (!matchesFilter(name, opts.filter)) continue;
        out += "wpo-stamp-fn " + name + " identity " + std::to_string(row.identity) + " family " +
               std::to_string(row.family) + " direct " + std::to_string(row.direct) + " poly " +
               std::to_string(row.poly) + "\n";
    }
    return out;
}

}  // namespace

std::string predictDirectTargets(il::Module& mod, const BuildOptions& opts,
                                 std::unordered_map<uint32_t, uint32_t>* ownCallees,
                                 std::unordered_map<uint32_t, uint32_t>* accessorTargets) {
    StampPlan plan;
    {
        Solver predicted(mod, solverOptions(opts, /*predict=*/true));
        predicted.run();
        ShapeAnalysis shapes(predicted);
        shapes.run();
        plan = planStamps(predicted, shapes, classifySites(predicted, shapes));
    }
    applyDirects(mod, plan);
    const auto collectUniqueByIc = [&](const std::vector<DirectEntry>& entries,
                                       std::unordered_map<uint32_t, uint32_t>* out) {
        if (out == nullptr) return;
        constexpr uint32_t kDisagree = UINT32_MAX;
        for (const DirectEntry& d : entries) {
            const uint32_t site = mod.functions[d.fn].blocks[d.block].instructions[d.inst].icIndex;
            auto [it, fresh] = out->insert({site, d.target});
            if (!fresh && it->second != d.target) it->second = kDisagree;
        }
        for (auto it = out->begin(); it != out->end();) {
            it = it->second == kDisagree ? out->erase(it) : std::next(it);
        }
    };
    collectUniqueByIc(plan.ownCalls, ownCallees);
    collectUniqueByIc(plan.accessorCalls, accessorTargets);
    if (!opts.stats) return {};
    std::string out = "wpo-early: predicted direct method targets " + std::to_string(plan.directs.size()) + " of " +
                      std::to_string(plan.callCandidates) +
                      " single-callee sites without one, before inlining (lowering's guess withdrawn at " +
                      std::to_string(plan.callWithdrawn) + "); refused: the callee may sit in an own slot " +
                      std::to_string(plan.callOwnHeld) + ", the callee's parameters " +
                      std::to_string(plan.callRefused) + "\n";
    out += "wpo-early: predicted own-slot callees " + std::to_string(plan.ownCalls.size()) + " sites, " +
           std::to_string(ownCallees != nullptr ? ownCallees->size() : 0) + " cache sites\n";
    if (!opts.perSite) return out;
    std::map<uint32_t, uint32_t> perFn;
    for (const DirectEntry& d : plan.directs) ++perFn[d.fn];
    for (const auto& [fn, n] : perFn) {
        const std::string& name = mod.functions[fn].name;
        if (matchesFilter(name, opts.filter)) out += "wpo-early-fn " + name + " direct " + std::to_string(n) + "\n";
    }
    return out;
}

std::string optimizeForBuild(il::Module& mod, const BuildOptions& opts) {
    StampPlan plan;
    {
        Solver predicted(mod, solverOptions(opts, /*predict=*/true));
        predicted.run();
        ShapeAnalysis shapes(predicted);
        shapes.run();
        shapes.upgradeInstanceInlineSlots(mod);
        plan = planStamps(predicted, shapes, classifySites(predicted, shapes));
    }
    std::string report;
    if (opts.stats) {
        // Both censuses read the module as lowering left it, so what they
        // report beside a site is lowering's own stamp.
        report += census(mod, opts, /*predict=*/false, nullptr);
        report += census(mod, opts, /*predict=*/true, &plan);
        report += stampReport(mod, plan, opts);
    }
    applyStamps(mod, plan);
    return report;
}

}  // namespace bronze::wpo
