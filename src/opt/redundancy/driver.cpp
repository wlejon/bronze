// The redundancy pass's driver: the job order within a round, the rounds to a
// fixed point, and the module walk.

#include <algorithm>
#include <map>
#include <vector>

#include "opt/redundancy/jobs.h"
#include "opt/redundancy/redundancy.h"

namespace bronze::opt {

void RedundancyStats::add(const RedundancyStats& o) {
    rounds += o.rounds;
    valuesNumbered += o.valuesNumbered;
    constantsFolded += o.constantsFolded;
    identitiesFolded += o.identitiesFolded;
    testsFolded += o.testsFolded;
    branchesFolded += o.branchesFolded;
    blocksRemoved += o.blocksRemoved;
    loadsEliminated += o.loadsEliminated;
    loadsForwarded += o.loadsForwarded;
    hoisted += o.hoisted;
    guardsHoisted += o.guardsHoisted;
    deadStores += o.deadStores;
    deadInstructions += o.deadInstructions;
    deadBlockParams += o.deadBlockParams;
    trivialBlockParams += o.trivialBlockParams;
    forwardersRemoved += o.forwardersRemoved;
    resumePointsRemoved += o.resumePointsRemoved;
}

namespace {

using redundancy::JobContext;

// The jobs that only remove or rewrite in place, cheapest first.
bool runCleanup(JobContext& ctx) {
    bool changed = redundancy::runValueWalk(ctx);
    changed |= redundancy::runDeadStores(ctx);
    changed |= redundancy::runDeadCode(ctx);
    return changed;
}

// Every resume point id a `leave` of some speculated function names, by the
// generic function it names it in. Ordered containers: the ids come out
// sorted and the walk is a function of the module alone.
std::map<uint32_t, std::vector<uint32_t>> namedResumeIds(const il::Module& module) {
    std::map<uint32_t, std::vector<uint32_t>> named;
    for (const il::Function& fn : module.functions) {
        if (!fn.isSpeculated()) continue;
        for (const il::Block& block : fn.blocks) {
            if (block.instructions.empty()) continue;
            const il::Instruction& term = block.instructions.back();
            if (term.op != il::Op::Leave || term.immI32 < 0) continue;
            named[term.calleeIndex].push_back(static_cast<uint32_t>(term.immI32));
        }
    }
    for (auto& [g, ids] : named) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
    return named;
}

}  // namespace

bool runRedundancy(il::Module& module, il::Function& fn, const RedundancyOptions& options,
                   RedundancyStats* stats) {
    if (fn.isExternal || fn.blocks.empty()) return false;
    RedundancyStats local;
    JobContext ctx{module, fn, options, stats != nullptr ? *stats : local};

    // Unnamed resume points first: each is a barrier the other jobs stop at.
    bool any = redundancy::removeUnnamedResumePoints(ctx);
    bool lastRoundChanged = false;
    for (uint32_t round = 0; round < options.maxRounds; ++round) {
        ++ctx.stats.rounds;
        bool changed = runCleanup(ctx);
        // Last: it is the most expensive job, and the cleanup before it hands
        // it fewer instructions and folded guards. What it moves is cleaned up
        // by the next round.
        changed |= redundancy::runLoopHoisting(ctx);
        lastRoundChanged = changed;
        any |= changed;
        if (!changed) break;
    }
    // The bound cut the rounds short: what the last round's hoisting left
    // behind (the guards it made redundant, empty blocks) still goes.
    if (lastRoundChanged && options.maxRounds > 0) any |= runCleanup(ctx);
    return any;
}

RedundancyStats runRedundancyModule(il::Module& module, const RedundancyOptions& options) {
    RedundancyStats stats;
    // Speculated functions first: the leaves they keep decide which resume
    // points of G stay. Indices, not iterators: the deque does not grow here,
    // but nothing below should have to know that.
    for (size_t i = 0; i < module.functions.size(); ++i) {
        il::Function& fn = module.functions[i];
        if (fn.isSpeculated()) runRedundancy(module, fn, options, &stats);
    }
    const std::map<uint32_t, std::vector<uint32_t>> named = namedResumeIds(module);
    for (size_t i = 0; i < module.functions.size(); ++i) {
        il::Function& fn = module.functions[i];
        if (fn.isSpeculated()) continue;
        RedundancyOptions forG = options;
        const auto it = named.find(static_cast<uint32_t>(i));
        forG.namedResumeIds = it != named.end() ? it->second : std::vector<uint32_t>{};
        runRedundancy(module, fn, forG, &stats);
    }
    return stats;
}

}  // namespace bronze::opt
