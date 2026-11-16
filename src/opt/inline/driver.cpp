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
//   - a MEDIUM callee (at most kMediumCallee) only in a loop (or as the
//     constructor of a speculated `new`), and only within
//     the caller's growth budget: kGrowthFactor times its size before the
//     pass, at least kMinGrowth more, never past kMaxCallerSize;
//   - nothing bigger.
// "The caller is hot" is no separate test: only a function hot enough to be
// speculated is ever a caller here (opt/speculate evidence.h).
//
// Application. The chosen calls are spliced back to front within each block
// (splice.cpp keeps everything in front of a site and every other block where
// it was), so the places recorded before the first splice stay right. A
// self-call splices a snapshot of the caller taken before the first splice:
// the body as it was, and its own self-call stays a call.

#include <algorithm>
#include <cassert>
#include <optional>
#include <utility>
#include <vector>

#include "opt/core/cfg.h"
#include "opt/core/loops.h"
#include "opt/inline/inline.h"
#include "opt/inline/parts.h"

namespace bronze::opt::inliner {

namespace {

// Instructions outside exit blocks (parts.h bodySize).
constexpr uint32_t kSmallCallee = 24;
constexpr uint32_t kMediumCallee = 120;
constexpr uint32_t kGrowthFactor = 4;
constexpr uint32_t kMinGrowth = 96;
constexpr uint32_t kMaxCallerSize = 8000;

struct Thresholds {
    uint32_t small = kSmallCallee;
    uint32_t medium = kMediumCallee;
};

// The speculated functions in post-order of the direct calls among them.
std::vector<uint32_t> bottomUpOrder(const il::Module& module) {
    const auto n = static_cast<uint32_t>(module.functions.size());
    std::vector<std::vector<uint32_t>> callees(n);
    for (uint32_t f = 0; f < n; ++f) {
        const il::Function& fn = module.functions[f];
        if (!fn.isSpeculated()) continue;
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (inst.op != il::Op::Call || inst.calleeIndex >= n || inst.calleeIndex == f) continue;
                if (module.functions[inst.calleeIndex].isSpeculated()) callees[f].push_back(inst.calleeIndex);
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

struct Candidate {
    BlockId block = kNoBlock;
    uint32_t index = 0;
    uint32_t callee = 0;
    uint32_t size = 0;
    uint32_t loopDepth = 0;
    const AfterCall* after = nullptr;
};

// The calls of `caller` (module function `c`) that may take their callee's
// body, with what the ranking needs.
std::vector<Candidate> candidatesOf(const il::Module& module, uint32_t c, const AfterCallPoints& points,
                                    InlineStats& stats) {
    const il::Function& caller = module.functions[c];
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
            const il::Function& callee = module.functions[call.calleeIndex];
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
            out.push_back(Candidate{b, i, call.calleeIndex, bodySize(callee), depth, after});
        }
    }
    return out;
}

// Ranks and takes the candidates under the policy; returns the chosen in
// splice order (block ascending, index descending).
std::vector<Candidate> choose(std::vector<Candidate> candidates, uint32_t callerSize, const Thresholds& t,
                              InlineStats& stats) {
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.loopDepth != b.loopDepth) return a.loopDepth > b.loopDepth;
        if (a.size != b.size) return a.size < b.size;
        if (a.block != b.block) return a.block < b.block;
        return a.index < b.index;
    });
    const uint32_t budget =
        std::min(kMaxCallerSize, std::max(callerSize * kGrowthFactor, callerSize + kMinGrowth));
    uint32_t size = callerSize;
    std::vector<Candidate> chosen;
    for (const Candidate& cand : candidates) {
        const bool small = cand.size <= t.small;
        const bool medium = !small && cand.size <= t.medium && cand.loopDepth > 0;
        if (!small && !medium) {
            ++stats.refusedSize;
            continue;
        }
        const uint32_t limit = small ? kMaxCallerSize : budget;
        if (size + cand.size > limit) {
            ++stats.refusedBudget;
            continue;
        }
        size += cand.size;
        chosen.push_back(cand);
    }
    std::sort(chosen.begin(), chosen.end(), [](const Candidate& a, const Candidate& b) {
        if (a.block != b.block) return a.block < b.block;
        return a.index > b.index;
    });
    return chosen;
}

void inlineInto(il::Module& module, uint32_t c, const Thresholds& t, InlineStats& stats) {
    il::Function& caller = module.functions[c];
    assert(caller.isSpeculated());
    ++stats.callersVisited;
    // G_c is not edited by this pass, so the points and their predecessors
    // read here hold for every splice into S_c.
    const AfterCallPoints points(module.functions[caller.speculatedFrom]);
    const std::vector<Candidate> chosen =
        choose(candidatesOf(module, c, points, stats), bodySize(caller), t, stats);
    if (chosen.empty()) return;

    std::optional<il::Function> self;
    for (const Candidate& cand : chosen) {
        if (cand.callee == c) {
            self = caller;
            break;
        }
    }
    for (const Candidate& cand : chosen) {
        const il::Function& callee = cand.callee == c ? *self : module.functions[cand.callee];
        const SpliceResult r = spliceCall(module, c, cand.block, cand.index, callee, cand.after);
        ++stats.inlined;
        if (callee.isSpeculated()) ++stats.inlinedSpeculated;
        stats.leavesRewritten += r.leavesRewritten;
        stats.instructionsAdded += r.instructionsAdded;
    }
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
        Thresholds t;
        if (options.smallCallee != 0) t.small = options.smallCallee;
        if (options.mediumCallee != 0) t.medium = options.mediumCallee;
        for (uint32_t c : bottomUpOrder(module)) inlineInto(module, c, t, stats);
    }
    clearResumeAfter(module);
    return stats;
}

}  // namespace bronze::opt::inliner
