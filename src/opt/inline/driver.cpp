// THE DRIVER: which callers, in which order, which of their calls, and the
// clearing of `resumeAfter` at the end (inline.h has the design).
//
// Order. Callers are the speculated functions, taken in post-order of the
// graph of direct calls between them (callee before caller; a cycle is broken
// where the depth-first walk meets it, by function index), so a callee is
// copied with its own inlining done and the one leave rule suffices.
//
// Choice, per caller. Every direct call is a candidate if its callee may be
// inlined at all and the call can take it (callee.cpp). Candidates are ranked
// by loop depth (deepest first), then by size (smallest first), then by place
// (block, index) for determinism, and taken greedily:
//   - a SMALL callee (at most kSmallCallee) whenever the caller stays within
//     kMaxCallerSize: a body that small costs about what the call costs;
//   - a LARGER callee (at most kLargeCallee) only within the caller's growth
//     budget: kGrowthFactor times its size before the pass, at least
//     kMinGrowth more, never past kMaxCallerSize; deeper loops take it first;
//   - a HOT call (below) takes a callee of up to kHotCallee within a budget
//     of at least kHotGrowth more;
//   - a call the harvested run never made takes a small callee only: a body
//     no one ran is code size and live ranges for nothing;
//   - nothing bigger.
// A callee's size is its body's once cleaned up (InlineOptions::cleanup).
// "The caller is hot" is no separate test: only a function hot enough to be
// speculated is ever a caller here (opt/speculate evidence.h).
//
// Weights. A harvest counts every call (il/feedback_site.h kCallCountArm)
// and every function's entries; a direct call keeps the count of the generic
// call it stands for, and the arms of a polymorphic site share it by their
// callees' entries (callWeight). A call is HOT when its weight is at least
// 1/kHotShare of all the speculated functions' direct calls' weights: a
// share, not a count, so a harvest run's length does not matter. A hot call
// is in a hot caller, so the larger budget is the caller's hotness too.
// Without counts (no harvest, a stale one) every call is lukewarm. Measured
// on the bench scenes against the same harvests: pixi_sprites -5%
// instructions, three_skinned -2%, three_meshes -1%, the others flat
// (shares of 1/500 and 1/2000 within noise of 1/1000); the same limits for
// every call were a wash (three_lights +6%).
//
// Optimistic copies (opt/speculate optimistic.h). A call names its callee's
// entry, S+ where there is one. A conservative caller (an S, or a function
// with no S+) takes the callee's S instead (bodyOf): an optimistic exit in S
// would count toward retiring S to G. An optimistic caller takes S+. A
// conservative caller with no S+ of its own that would inline an optimistic
// body if it were optimistic gets one here: a copy of it before any splice
// (fallback: the caller), which takes the optimistic bodies; every reference
// to the caller is pointed at the copy when the pass ends. Weights are summed
// over the conservative functions only, so a second copy of a function does
// not count its calls twice.
//
// Application. The chosen calls are spliced back to front within each block
// (splice.cpp keeps everything in front of a site and every other block where
// it was), so the places recorded before the first splice stay right. A
// self-call splices a snapshot of the caller taken before the first splice:
// the body as it was, and its own self-call stays a call.

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "il/feedback_site.h"
#include "opt/core/cfg.h"
#include "opt/core/clone.h"
#include "opt/core/loops.h"
#include "opt/inline/inline.h"
#include "opt/inline/parts.h"

namespace bronze::opt::inliner {

namespace {

// Instructions outside exit blocks (parts.h bodySize).
constexpr uint32_t kSmallCallee = 24;
constexpr uint32_t kLargeCallee = 400;
constexpr uint32_t kHotCallee = 1000;
constexpr uint64_t kHotShare = 1000;
constexpr uint32_t kGrowthFactor = 4;
constexpr uint32_t kMinGrowth = 800;
constexpr uint32_t kHotGrowth = 3000;
constexpr uint32_t kMaxCallerSize = 8000;

// The body a direct call of function `callee` made by `caller` takes: a
// conservative caller's is the callee's conservative copy, an optimistic
// caller's the callee's optimistic one, the copy the pass made (`plus`) when
// it made one.
uint32_t bodyOf(const il::Module& module, const il::Function& caller, uint32_t callee,
                const std::unordered_map<uint32_t, uint32_t>& plus) {
    if (callee >= module.functions.size()) return callee;
    if (caller.isOptimistic()) {
        const auto it = plus.find(callee);
        return it != plus.end() ? it->second : callee;
    }
    const il::Function& f = module.functions[callee];
    return f.isOptimistic() ? f.fallback : callee;
}

// The speculated functions in post-order of the direct calls among them.
std::vector<uint32_t> bottomUpOrder(const il::Module& module) {
    const auto n = static_cast<uint32_t>(module.functions.size());
    std::vector<std::vector<uint32_t>> callees(n);
    for (uint32_t f = 0; f < n; ++f) {
        const il::Function& fn = module.functions[f];
        if (!fn.isSpeculated()) continue;
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (inst.op != il::Op::Call || inst.calleeIndex >= n) continue;
                // A conservative caller may get an optimistic copy taking the
                // callee's optimistic body (inlineInto), so that body comes
                // first too.
                for (const uint32_t body : {bodyOf(module, fn, inst.calleeIndex, {}), inst.calleeIndex}) {
                    if (body != f && module.functions[body].isSpeculated()) callees[f].push_back(body);
                }
            }
        }
        std::sort(callees[f].begin(), callees[f].end());
        callees[f].erase(std::unique(callees[f].begin(), callees[f].end()), callees[f].end());
    }

    // Iterative depth-first walk: a call graph can be deep, a stack frame per
    // level is not worth the risk.
    std::vector<uint32_t> order;
    std::vector<uint8_t> seen(n, 0);
    std::vector<std::pair<uint32_t, size_t>> stack;
    for (uint32_t root = 0; root < n; ++root) {
        if (seen[root] || !module.functions[root].isSpeculated()) continue;
        seen[root] = 1;
        stack.emplace_back(root, 0);
        while (!stack.empty()) {
            auto& [f, next] = stack.back();
            if (next < callees[f].size()) {
                const uint32_t c = callees[f][next++];
                if (!seen[c]) {
                    seen[c] = 1;
                    stack.emplace_back(c, 0);
                }
                continue;
            }
            order.push_back(f);
            stack.pop_back();
        }
    }
    return order;
}

// The harvested run's count of the generic call a direct call stands for.
std::optional<uint64_t> callCount(const InlineOptions& options, const il::Instruction& call) {
    if (!options.harvestCount || call.slowSite == il::Instruction::kNoSlowSite) return std::nullopt;
    return options.harvestCount(call.slowSite + il::kCallCountArm);
}

// The harvested run's entries of function `f` (il.h Function::entrySlowSite).
std::optional<uint64_t> entryCount(const il::Module& module, const InlineOptions& options, uint32_t f) {
    if (!options.harvestCount || f >= module.functions.size()) return std::nullopt;
    const uint32_t site = module.functions[f].entrySlowSite;
    if (site == il::Instruction::kNoSlowSite) return std::nullopt;
    return options.harvestCount(site);
}

// How often THIS direct call ran: its generic call's count, shared out over
// the arms of a polymorphic site (every direct call of `fn` standing for the
// same generic call, each to another callee) by their callees' entries.
std::optional<uint64_t> callWeight(const il::Module& module, const il::Function& fn, const InlineOptions& options,
                                   const il::Instruction& call) {
    const std::optional<uint64_t> count = callCount(options, call);
    if (!count || *count == 0) return count;
    std::vector<uint32_t> arms;
    for (const il::Block& block : fn.blocks) {
        for (const il::Instruction& inst : block.instructions) {
            if (inst.op == il::Op::Call && inst.slowSite == call.slowSite &&
                std::find(arms.begin(), arms.end(), inst.calleeIndex) == arms.end()) {
                arms.push_back(inst.calleeIndex);
            }
        }
    }
    if (arms.size() <= 1) return count;
    uint64_t sum = 0;
    std::optional<uint64_t> mine;
    for (uint32_t f : arms) {
        const std::optional<uint64_t> e = entryCount(module, options, f);
        if (!e) return *count / arms.size();
        sum += *e;
        if (f == call.calleeIndex) mine = e;
    }
    if (sum == 0 || !mine) return *count / arms.size();
    return static_cast<uint64_t>(static_cast<long double>(*count) * static_cast<long double>(*mine) /
                                 static_cast<long double>(sum));
}

struct Candidate {
    BlockId block = kNoBlock;
    uint32_t index = 0;
    uint32_t callee = 0;
    uint32_t size = 0;
    uint32_t loopDepth = 0;
    const AfterCall* after = nullptr;
    std::optional<uint64_t> count;  // the call's weight (callWeight), from InlineOptions::harvestCount
};

// The calls of `caller` (module function `c`) that may take their callee's
// body, with what the ranking needs.
std::vector<Candidate> candidatesOf(const il::Module& module, const il::Function& caller,
                                    const AfterCallPoints& points, const InlineOptions& options,
                                    const std::unordered_map<uint32_t, uint32_t>& plus, InlineStats& stats) {
    const Cfg cfg(caller);
    const DomTree dom(cfg);
    const LoopForest loops(cfg, dom);
    std::vector<Candidate> out;
    for (BlockId b = 0; b < caller.blocks.size(); ++b) {
        // A call no path reaches, or one on a path lowering expects not to
        // take (or an exit's), gains nothing from a body worth its size.
        if (!cfg.reachable(b) || caller.blocks[b].cold) continue;
        const std::vector<il::Instruction>& insts = caller.blocks[b].instructions;
        for (uint32_t i = 0; i + 1 < insts.size(); ++i) {
            const il::Instruction& call = insts[i];
            if (call.op != il::Op::Call || call.calleeIndex >= module.functions.size()) continue;
            ++stats.sitesConsidered;
            const uint32_t body = bodyOf(module, caller, call.calleeIndex, plus);
            const il::Function& callee = module.functions[body];
            if (!calleeInlinable(callee)) {
                ++stats.refusedCallee;
                continue;
            }
            const AfterCall* after =
                call.resumeAfter != il::Instruction::kNoResumeAfter ? points.find(call.resumeAfter) : nullptr;
            assert((call.resumeAfter == il::Instruction::kNoResumeAfter || after != nullptr) &&
                   "a resumeAfter names a point right after a generic call");
            if (!siteFits(call, callee, after)) {
                ++stats.refusedSite;
                continue;
            }
            // A speculated `new` (opt/speculate construct.cpp) calls its
            // constructor on the instance made just before: inlined, the
            // constructor's adds fold against the instance's known shape, so
            // it is taken as a call in a loop would be.
            const bool construct = i > 0 && insts[i - 1].op == il::Op::CreateThis && callee.needsThis &&
                                   call.operands[callee.needsEnv ? 1 : 0] == insts[i - 1].result;
            const uint32_t depth = construct ? std::max<uint32_t>(loops.depth(b), 1) : loops.depth(b);
            Candidate cand{b, i, body, bodySize(callee), depth, after, std::nullopt};
            cand.count = callWeight(module, caller, options, call);
            out.push_back(cand);
        }
    }
    return out;
}

// BRONZE_INLINE_TRACE=<file>: one line per candidate, appended: caller,
// callee, the callee's size, the loop depth, the call's weight ('-' for
// none), what was decided, and the caller's size so far. A diagnostic.
std::FILE* traceFile() {
    static std::FILE* f = [] {
        const char* path = std::getenv("BRONZE_INLINE_TRACE");
        return path != nullptr && *path != 0 ? std::fopen(path, "a") : nullptr;
    }();
    return f;
}

void trace(const il::Module& module, uint32_t c, uint32_t callerSize, const Candidate& cand, const char* what) {
    std::FILE* f = traceFile();
    if (f == nullptr) return;
    std::fprintf(f, "%s\t%s\t%u\t%u\t%s\t%s\t%u\n", module.functions[c].name.c_str(),
                 module.functions[cand.callee].name.c_str(), cand.size, cand.loopDepth,
                 cand.count ? std::to_string(*cand.count).c_str() : "-", what, callerSize);
}

// Ranks and takes the candidates under the policy; returns the chosen in
// splice order (block ascending, index descending). `total` is the weight of
// every direct call of the speculated functions (callWeight).
std::vector<Candidate> choose(const il::Module& module, uint32_t c, std::vector<Candidate> candidates,
                              uint32_t callerSize, uint64_t total, InlineStats& stats) {
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.loopDepth != b.loopDepth) return a.loopDepth > b.loopDepth;
        if (a.size != b.size) return a.size < b.size;
        if (a.block != b.block) return a.block < b.block;
        return a.index < b.index;
    });
    const auto budgetOf = [callerSize](uint32_t growth) {
        return std::min(kMaxCallerSize, std::max(callerSize * kGrowthFactor, callerSize + growth));
    };
    const uint32_t budget = budgetOf(kMinGrowth);
    const uint32_t hotBudget = budgetOf(kHotGrowth);
    uint32_t size = callerSize;
    std::vector<Candidate> chosen;
    for (const Candidate& cand : candidates) {
        const bool small = cand.size <= kSmallCallee;
        const bool hot = cand.count && total != 0 && *cand.count * kHotShare >= total;
        const bool neverRan = cand.count && *cand.count == 0;
        if (!small && (neverRan || cand.size > (hot ? kHotCallee : kLargeCallee))) {
            ++stats.refusedSize;
            trace(module, c, size, cand, neverRan ? "never-ran" : "size");
            continue;
        }
        const uint32_t limit = small ? kMaxCallerSize : (hot ? hotBudget : budget);
        if (size + cand.size > limit) {
            ++stats.refusedBudget;
            trace(module, c, size, cand, "budget");
            continue;
        }
        size += cand.size;
        if (hot) ++stats.inlinedHot;
        chosen.push_back(cand);
        trace(module, c, size, cand, hot ? "inline-hot" : "inline");
    }
    std::sort(chosen.begin(), chosen.end(), [](const Candidate& a, const Candidate& b) {
        if (a.block != b.block) return a.block < b.block;
        return a.index > b.index;
    });
    return chosen;
}

// Callee sizes as the policy reads them (InlineOptions::cleanup), each
// measured once: a callee's body no longer changes once the bottom-up walk
// has passed it.
class CalleeSizes {
public:
    CalleeSizes(const il::Module& module, const InlineOptions& options) : module_(module), options_(options) {}
    uint32_t of(uint32_t f) {
        const auto it = sizes_.find(f);
        if (it != sizes_.end()) return it->second;
        uint32_t size = 0;
        if (options_.cleanup) {
            il::Function copy = module_.functions[f];
            options_.cleanup(copy);
            size = bodySize(copy);
        } else {
            size = bodySize(module_.functions[f]);
        }
        sizes_.emplace(f, size);
        return size;
    }
    void forget(uint32_t f) { sizes_.erase(f); }

private:
    const il::Module& module_;
    const InlineOptions& options_;
    std::unordered_map<uint32_t, uint32_t> sizes_;
};

// One run of the pass: what every caller shares, and the optimistic copies
// it made (caller -> copy).
struct Run {
    il::Module& module;
    CalleeSizes& sizes;
    const InlineOptions& options;
    uint64_t total = 0;
    InlineStats& stats;
    std::unordered_map<uint32_t, uint32_t> plus;
    std::vector<uint8_t> hasCopy;  // by index: a speculated function S+ falls back to
};

// The calls of `caller` (module function `c`, or a copy of it about to be:
// `c` names the caller only in the trace) the policy takes, in splice order.
std::vector<Candidate> chosenFor(Run& run, const il::Function& caller, uint32_t c, const AfterCallPoints& points,
                                 InlineStats& stats) {
    std::vector<Candidate> candidates = candidatesOf(run.module, caller, points, run.options, run.plus, stats);
    for (Candidate& cand : candidates) cand.size = run.sizes.of(cand.callee);
    return choose(run.module, c, std::move(candidates), bodySize(caller), run.total, stats);
}

void spliceChosen(Run& run, uint32_t c, const std::vector<Candidate>& chosen) {
    std::optional<il::Function> self;
    for (const Candidate& cand : chosen) {
        if (cand.callee == c) {
            self = run.module.functions[c];
            break;
        }
    }
    for (const Candidate& cand : chosen) {
        const il::Function& callee = cand.callee == c ? *self : run.module.functions[cand.callee];
        const SpliceResult r = spliceCall(run.module, c, cand.block, cand.index, callee, cand.after);
        ++run.stats.inlined;
        if (callee.isSpeculated()) ++run.stats.inlinedSpeculated;
        if (callee.isOptimistic()) ++run.stats.inlinedOptimistic;
        run.stats.leavesRewritten += r.leavesRewritten;
        run.stats.instructionsAdded += r.instructionsAdded;
    }
}

void inlineInto(Run& run, uint32_t c) {
    il::Module& module = run.module;
    assert(module.functions[c].isSpeculated());
    ++run.stats.callersVisited;
    // The points after G_c's calls. The pass adds only optimistic points to
    // G_c (splice.cpp), each right after an ordinary one, which leaves every
    // after-call point where this finds it.
    const AfterCallPoints points(module.functions[module.functions[c].speculatedFrom]);
    const std::vector<Candidate> chosen = chosenFor(run, module.functions[c], c, points, run.stats);

    // A conservative caller with no optimistic copy: would the copy take an
    // optimistic body? The probe is the caller before any splice, as its
    // copy would be.
    std::optional<il::Function> probe;
    std::vector<Candidate> probeChosen;
    InlineStats probeStats;
    const uint32_t copyIndex = static_cast<uint32_t>(module.functions.size());
    if (!module.functions[c].isOptimistic() && (c >= run.hasCopy.size() || !run.hasCopy[c])) {
        probe = module.functions[c];
        probe->fallback = c;
        probe->name = module.functions[probe->speculatedFrom].name + "$opt";
        // A self-call of the copy takes the caller's body as spliced (the
        // copy is not in the module while it is chosen for).
        probeChosen = chosenFor(run, *probe, c, points, probeStats);
        const bool optimistic = std::any_of(probeChosen.begin(), probeChosen.end(), [&](const Candidate& cand) {
            return module.functions[cand.callee].isOptimistic();
        });
        if (!optimistic) probe.reset();
    }

    spliceChosen(run, c, chosen);
    if (!probe) return;
    module.functions.push_back(std::move(*probe));
    run.plus[c] = copyIndex;
    ++run.stats.optimisticCopies;
    ++run.stats.callersVisited;
    run.stats.sitesConsidered += probeStats.sitesConsidered;
    run.stats.refusedCallee += probeStats.refusedCallee;
    run.stats.refusedSite += probeStats.refusedSite;
    run.stats.refusedSize += probeStats.refusedSize;
    run.stats.refusedBudget += probeStats.refusedBudget;
    run.stats.inlinedHot += probeStats.inlinedHot;
    spliceChosen(run, copyIndex, probeChosen);
}

// `resumeAfter` is the inliner's alone; past it a point named only by one is
// garbage the redundancy pass deletes, and the verifier would then see a
// call naming a point that is gone.
void clearResumeAfter(il::Module& module) {
    for (il::Function& fn : module.functions) {
        for (il::Block& block : fn.blocks) {
            for (il::Instruction& inst : block.instructions) inst.resumeAfter = il::Instruction::kNoResumeAfter;
        }
    }
}

}  // namespace

InlineStats inlineModule(il::Module& module, const InlineOptions& options) {
    InlineStats stats;
    if (options.enabled) {
        CalleeSizes sizes(module, options);
        Run run{module, sizes, options, 0, stats, {}, std::vector<uint8_t>(module.functions.size(), 0)};
        uint64_t& total = run.total;
        for (const il::Function& fn : module.functions) {
            if (fn.isOptimistic()) run.hasCopy[fn.fallback] = 1;
            // An optimistic copy's calls are its conservative copy's again.
            if (!fn.isSpeculated() || fn.isOptimistic()) continue;
            for (const il::Block& block : fn.blocks) {
                for (const il::Instruction& inst : block.instructions) {
                    if (inst.op != il::Op::Call) continue;
                    if (const std::optional<uint64_t> n = callWeight(module, fn, options, inst)) total += *n;
                }
            }
        }
        for (uint32_t c : bottomUpOrder(module)) {
            inlineInto(run, c);
            sizes.forget(c);
        }
        // Every reference to a caller the pass made an optimistic copy of
        // names the copy: it is entered first (opt/speculate optimistic.h).
        if (!run.plus.empty()) {
            std::vector<uint32_t> entry(module.functions.size());
            for (uint32_t f = 0; f < entry.size(); ++f) entry[f] = f;
            for (const auto& [c, copy] : run.plus) entry[c] = copy;
            redirectFunctionReferences(module, entry);
        }
    }
    clearResumeAfter(module);
    return stats;
}

}  // namespace bronze::opt::inliner
