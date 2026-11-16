// The optimiser pipeline (pipeline.h says what runs and in which order).

#include "opt/pipeline/pipeline.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "il/print.h"
#include "il/verifier.h"

namespace bronze::opt {

namespace {

bool verified(const il::Module& module, const OptimizeOptions& options, const char* when) {
    if (!options.verify) return true;
    DiagnosticSink local;
    DiagnosticSink& diags = options.diags ? *options.diags : local;
    if (il::verify(module, diags)) return true;
    // The verifier's own messages say what; this says where in the pipeline.
    diags.error(Span{}, std::string("IL verification failed ") + when);
    // BRONZE_OPT_FAIL_DUMP=<file>: the module as the failing stage left it.
    if (const char* path = std::getenv("BRONZE_OPT_FAIL_DUMP"); path != nullptr && *path != 0) {
        if (std::FILE* f = std::fopen(path, "w")) {
            std::fputs(il::print(module).c_str(), f);
            std::fclose(f);
        }
    }
    return false;
}

// The redundancy pass's PreheaderResumeFn, answered by the speculation step's
// loop-entry resume points. loopEntryResume answers only with a state whose
// every value has G's type at the resume point (speculate.h), so the state is
// passed as it is: nothing to box in the exit block.
PreheaderResumeFn loopEntryResumes(const il::Module& module) {
    return [&module](const il::Function& fn, il::BlockId header, il::BlockId preheader,
                     const std::function<il::ValueId(il::ValueId)>& current) -> std::optional<LeaveTarget> {
        std::optional<speculate::LoopEntryResume> entry =
            speculate::loopEntryResume(module, fn, header, preheader, current);
        if (!entry) return std::nullopt;
        LeaveTarget target;
        target.resumeId = entry->resumeId;
        target.buildState = [state = std::move(entry->stateInS)](il::Function&, il::BlockId) {
            return state;
        };
        return target;
    };
}

}  // namespace

OptimizeResult optimizeModule(il::Module& module, const spec::TypeProfile* profile,
                              const OptimizeOptions& options) {
    OptimizeResult result;
    if (!verified(module, options, "on the module as lowered")) {
        result.ok = false;
        return result;
    }

    if (profile != nullptr) {
        result.profileRead = true;
        result.speculate = speculate::speculateModule(module, *profile, options.speculate);
    }
    const bool anySpeculated = result.speculate.functionsSpeculated != 0;

    // Always run: it also clears every call's resumeAfter, which the
    // redundancy pass would otherwise leave naming a point it deletes.
    if (anySpeculated && !verified(module, options, "after speculation")) {
        result.ok = false;
        return result;
    }
    // The inliner sizes a callee by a copy the redundancy pass cleaned up,
    // which is what the body costs once its caller is cleaned up.
    RedundancyOptions sizing;
    sizing.maxRounds = options.maxRounds;
    sizing.hoistLoops = false;
    inliner::InlineOptions inlining = options.inlining;
    inlining.cleanup = [&module, sizing](il::Function& fn) { runRedundancy(module, fn, sizing); };
    result.inlined = inliner::inlineModule(module, inlining);
    if (!verified(module, options, "after inlining")) {
        result.ok = false;
        return result;
    }

    RedundancyOptions redundancy;
    redundancy.maxRounds = options.maxRounds;
    redundancy.hoistLoops = options.hoistLoops;
    // runRedundancyModule fills namedResumeIds per G from the surviving
    // leaves; without a speculated function there is no resume point to drop.
    redundancy.removeUnnamedResumePoints = true;
    if (anySpeculated) redundancy.preheaderResume = loopEntryResumes(module);
    result.redundancy = runRedundancyModule(module, redundancy);
    if (!verified(module, options, "after the redundancy pass")) {
        result.ok = false;
        return result;
    }

    if (anySpeculated) result.repr = selectRepresentationsModule(module, redundancy);

    if (!verified(module, options, "after representation selection")) result.ok = false;
    return result;
}

std::string formatOptimizeStats(const OptimizeResult& r) {
    auto n = [](uint64_t v) { return std::to_string(v); };
    const speculate::SpeculateStats& s = r.speculate;
    std::string out;
    if (r.profileRead) {
        out += "speculate: " + n(s.functionsSpeculated) + "/" + n(s.functionsConsidered) + " functions";
        if (s.harvestIgnored) out += " (harvest of another build ignored)";
        out += "; sites: prop.get " + n(s.propReads) + " (chains " + n(s.propReadChains) + "), shape sets " + n(s.shapeSetLeads) + " (followers " + n(s.shapeSetFollowers) + "), slot-keyed " + n(s.propSlotKeyed) + " (absent " + n(s.propAbsentTests) + "), number slots " + n(s.numberSlotKeys) + " keys (reads " + n(s.numberSlotReads) + ", stores " + n(s.numberSlotStores) + "), prop.set " +
               n(s.propWrites) + ", add " + n(s.propAdds) + ", method " + n(s.methodCalls) + ", call " + n(s.plainCalls) + ", new " + n(s.constructs) + ", arith " +
               n(s.arithmetic) + ", elem.get " + n(s.elementReads) + ", elem.set " + n(s.elementWrites) + ", length " + n(s.arrayLengths) + ", push " + n(s.arrayPushes) + ", tdz " + n(s.tdzReads) + ", math " + n(s.mathCalls) +
               "; guards " + n(s.guards) + ", resume points " + n(s.resumePoints) + " (loop entry " +
               n(s.loopEntryPoints) + "), shape cells " + n(s.shapeCells) + ", redirected " + n(s.redirected) +
               "\n";
    } else {
        out += "speculate: no profile\n";
    }
    const inliner::InlineStats& in = r.inlined;
    out += "inline: " + n(in.inlined) + "/" + n(in.sitesConsidered) + " calls (speculated " +
           n(in.inlinedSpeculated) + "); refused callee " + n(in.refusedCallee) + ", site " + n(in.refusedSite) +
           ", size " + n(in.refusedSize) + ", budget " + n(in.refusedBudget) + "; leaves rewritten " +
           n(in.leavesRewritten) + "\n";
    auto redundancyLine = [&](const char* name, const RedundancyStats& d) {
        out += std::string(name) + ": rounds " + n(d.rounds) + "; numbered " + n(d.valuesNumbered) +
               ", folded " + n(d.constantsFolded) + "+" + n(d.identitiesFolded) + ", tests " +
               n(d.testsFolded) + ", branches " + n(d.branchesFolded) + ", blocks removed " +
               n(d.blocksRemoved) + ", loads " + n(d.loadsEliminated) + "+" + n(d.loadsForwarded) +
               " forwarded, hoisted " + n(d.hoisted) + " (guards " + n(d.guardsHoisted) + "), strengthened " + n(d.guardsStrengthened) + ", dead stores " +
               n(d.deadStores) + ", dead " + n(d.deadInstructions) + ", params " + n(d.deadBlockParams) + "+" +
               n(d.trivialBlockParams) + ", forwarders " + n(d.forwardersRemoved) + ", resume points removed " +
               n(d.resumePointsRemoved) + "\n";
    };
    redundancyLine("redundancy", r.redundancy);
    if (r.speculate.functionsSpeculated != 0) {
        const ReprStats& p = r.repr;
        out += "repr: rounds " + n(p.rounds) + "; tests " + n(p.testsFolded) + ", unboxes " + n(p.unboxesFolded) +
               ", conversions " + n(p.conversionsFolded) + ", compares " + n(p.comparesTyped) + ", params " +
               n(p.paramsUnboxed) + " (widened " + n(p.paramsWidened) + ", edge conversions " +
               n(p.edgeConversions) + "), truthy params " + n(p.paramsTruthy) + ", boxes sunk " + n(p.boxesSunk) + ", to exits " + n(p.boxesSunkToExits) +
               " (" + n(p.exitBoxClones) + " copies), dead " + n(p.deadRemoved) + "\n";
        redundancyLine("repr cleanup", p.redundancy);
    }
    if (!r.ok) out += "verification failed\n";
    return out;
}

}  // namespace bronze::opt
