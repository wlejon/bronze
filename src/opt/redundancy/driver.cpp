// The redundancy pass's driver: the job order within a round, the rounds to a
// fixed point, and the module walk.

#include <algorithm>
#include <functional>
#include <map>
#include <vector>

#include "opt/redundancy/jobs.h"
#include "opt/redundancy/redundancy.h"
#include "support/parallel.h"

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
    guardsStrengthened += o.guardsStrengthened;
    deadStores += o.deadStores;
    deadInstructions += o.deadInstructions;
    deadBlockParams += o.deadBlockParams;
    trivialBlockParams += o.trivialBlockParams;
    forwardersRemoved += o.forwardersRemoved;
    resumePointsRemoved += o.resumePointsRemoved;
}

namespace {

using redundancy::JobContext;

// A thread per this many instructions of the functions a walk runs on: below
// it, starting a thread costs more than it saves.
constexpr size_t kMinInstructionsPerThread = 20000;

// The jobs that only remove or rewrite in place, cheapest first.
bool runCleanup(JobContext& ctx) {
    bool changed = redundancy::runGuardStrengthening(ctx);
    changed |= redundancy::runBoundsWidening(ctx);
    changed |= redundancy::runValueWalk(ctx);
    changed |= redundancy::runDeadStores(ctx);
    changed |= redundancy::runDeadCode(ctx);
    return changed;
}

// Every resume point id a `leave` or a `resume.call` of some speculated
// function names, by the generic function it names it in: a resume.call (an
// inlined body's failed guard finishing the callee's call, opt/inline) starts
// the callee's G at the point just as a leave would. Ordered containers: the
// ids come out sorted and the walk is a function of the module alone.
std::map<uint32_t, std::vector<uint32_t>> namedResumeIds(const il::Module& module) {
    std::map<uint32_t, std::vector<uint32_t>> named;
    for (const il::Function& fn : module.functions) {
        if (!fn.isSpeculated()) continue;
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if ((inst.op != il::Op::Leave && inst.op != il::Op::ResumeCall) || inst.immI32 < 0) continue;
                named[inst.calleeIndex].push_back(static_cast<uint32_t>(inst.immI32));
            }
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
    // Runs `run(i)` on each function index of `which`, functions in
    // parallel, and adds their stats in index order.
    RedundancyStats stats;
    auto runAll = [&](const std::vector<uint32_t>& which, const std::function<void(uint32_t, RedundancyStats&)>& run) {
        std::vector<size_t> weights(which.size(), 0);
        size_t total = 0;
        for (size_t k = 0; k < which.size(); ++k) {
            for (const il::Block& block : module.functions[which[k]].blocks) weights[k] += block.instructions.size();
            total += weights[k];
        }
        std::vector<RedundancyStats> each(which.size());
        const size_t threads = threadsForWork(options.threads, which.size(), total, kMinInstructionsPerThread);
        parallelForEach(heaviestFirst(weights), threads, [&](size_t k) { run(which[k], each[k]); });
        for (const RedundancyStats& s : each) stats.add(s);
    };
    std::vector<uint32_t> speculated, generic;
    for (size_t i = 0; i < module.functions.size(); ++i) {
        (module.functions[i].isSpeculated() ? speculated : generic).push_back(static_cast<uint32_t>(i));
    }
    // Speculated functions first: the leaves they keep decide which resume
    // points of G stay. Indices, not iterators: the deque does not grow here,
    // but nothing below should have to know that.
    runAll(speculated, [&](uint32_t i, RedundancyStats& s) { runRedundancy(module, module.functions[i], options, &s); });
    const std::map<uint32_t, std::vector<uint32_t>> named = namedResumeIds(module);
    runAll(generic, [&](uint32_t i, RedundancyStats& s) {
        RedundancyOptions forG = options;
        const auto it = named.find(i);
        forG.namedResumeIds = it != named.end() ? it->second : std::vector<uint32_t>{};
        runRedundancy(module, module.functions[i], forG, &s);
    });
    return stats;
}

}  // namespace bronze::opt
