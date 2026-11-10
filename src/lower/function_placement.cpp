// FUNCTION PLACEMENT (function_placement.h).

#include "lower/function_placement.h"

#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lower/guard_region_cfg.h"
#include "opt/spec/feedback_plan.h"

namespace bronze::lower {

namespace {

// The hot set is the functions that hold this share of the run's samples (or
// entries), the most sampled first: the rest of what ran is the program's
// setup, which ran once, and placing it with the frame's code would spread
// the frame's code over megabytes again.
constexpr uint64_t kHotCoverPermille = 990;
// A chain stops growing past this many bytes of estimated code: what a
// caller and its callees share is the pages and lines they sit on, and a
// chain longer than a few pages no longer keeps a caller near the callee
// appended last (C3 bounds its clusters by the page size).
constexpr uint64_t kChainBytes = 4096;
// Bytes of machine code an IL instruction becomes, roughly: what the chain
// bound is measured in.
constexpr uint64_t kBytesPerInstruction = 12;
// A call inside a loop runs, for want of counts, this many times as often as
// one outside.
constexpr uint64_t kLoopWeight = 8;

bool isIcSite(const il::Instruction& inst) {
    return inst.op == il::Op::PropGet || inst.op == il::Op::PropSet || inst.op == il::Op::MethodCall ||
           inst.op == il::Op::MethodCallSpread;
}

// Whether the site's cache fills when it runs: a method site always (its
// cache resolves the callee, a direct edge included); a property site unless
// something else answers it (a static-slot claim's cell, an accessor target's
// cell pair, a guarded region's shape guard).
bool siteSpeaks(const il::Instruction& inst) {
    if (inst.op == il::Op::MethodCall || inst.op == il::Op::MethodCallSpread) return true;
    return inst.staticSlot == il::Instruction::kNoStaticSlot &&
           inst.directTarget == il::Instruction::kNoDirectTarget && !inst.shapeGuarded;
}

// The module functions a direct call edge of `inst` enters: a call's callee,
// a method or accessor site's direct target and its typed entry.
void directCallees(const il::Instruction& inst, std::vector<uint32_t>& out) {
    if (inst.op == il::Op::Call) {
        out.push_back(inst.calleeIndex);
        return;
    }
    if (isIcSite(inst)) {
        if (inst.typedTarget != il::Instruction::kNoDirectTarget) out.push_back(inst.typedTarget);
        if (inst.directTarget != il::Instruction::kNoDirectTarget) out.push_back(inst.directTarget);
    }
}

// The blocks of `fn` inside some loop.
std::vector<uint8_t> loopBlocks(const il::Function& fn) {
    std::vector<uint8_t> in(fn.blocks.size(), 0);
    if (fn.blocks.empty()) return in;
    const Cfg cfg = buildCfg(fn);
    for (il::BlockId b = 0; b < fn.blocks.size(); ++b) {
        if (cfg.rpoIndex[b] == UINT32_MAX) continue;
        for (il::BlockId s : cfg.succs[b]) {
            if (!cfg.dominates(s, b)) continue;
            for (il::BlockId m : naturalLoop(cfg, s, b)) in[m] = 1;
        }
    }
    return in;
}

bool isTopLevel(const il::Function& fn) {
    return fn.isEntryPoint || fn.name.rfind("main.seg", 0) == 0;
}

// C3 (Ottoni and Maher, "Optimizing Function Placement for Large-Scale
// Data-Center Applications", CGO 2017) over `hot`: each function, hottest
// first, appends its chain to the chain of the caller with the heaviest
// edges into it, while the two fit in kChainBytes; then the chains, densest
// (heat per byte) first. Deterministic: every tie goes to the lower index.
std::vector<uint32_t> callChainOrder(const std::vector<uint32_t>& hot, const std::vector<uint64_t>& heat,
                                     const std::vector<uint64_t>& bytes,
                                     std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint64_t>>& callers,
                                     uint32_t* chainCount) {
    std::vector<uint32_t> byHeat = hot;
    std::stable_sort(byHeat.begin(), byHeat.end(), [&](uint32_t a, uint32_t b) { return heat[a] > heat[b]; });
    std::unordered_map<uint32_t, uint32_t> chainOf;
    std::unordered_map<uint32_t, std::vector<uint32_t>> chain;
    std::unordered_map<uint32_t, uint64_t> chainBytes, chainHeat;
    for (uint32_t f : hot) {
        chainOf[f] = f;
        chain[f] = {f};
        chainBytes[f] = bytes[f];
        chainHeat[f] = heat[f];
    }
    for (uint32_t f : byHeat) {
        uint32_t best = UINT32_MAX;
        uint64_t bestWeight = 0;
        for (const auto& [caller, weight] : callers[f]) {
            if (weight > bestWeight || (weight == bestWeight && caller < best)) {
                best = caller;
                bestWeight = weight;
            }
        }
        if (best == UINT32_MAX) continue;
        const uint32_t into = chainOf[best], from = chainOf[f];
        if (into == from || chainBytes[into] + chainBytes[from] > kChainBytes) continue;
        for (uint32_t g : chain[from]) {
            chain[into].push_back(g);
            chainOf[g] = into;
        }
        chain[from].clear();
        chainBytes[into] += chainBytes[from];
        chainHeat[into] += chainHeat[from];
    }
    std::vector<uint32_t> heads;
    for (uint32_t f : hot) {
        if (chainOf[f] == f) heads.push_back(f);
    }
    // Heat per byte, compared without division: a/b > c/d as a*d > c*b.
    std::stable_sort(heads.begin(), heads.end(), [&](uint32_t a, uint32_t b) {
        const double l = static_cast<double>(chainHeat[a]) * static_cast<double>(std::max<uint64_t>(chainBytes[b], 1));
        const double r = static_cast<double>(chainHeat[b]) * static_cast<double>(std::max<uint64_t>(chainBytes[a], 1));
        return l > r;
    });
    std::vector<uint32_t> out;
    for (uint32_t h : heads) out.insert(out.end(), chain[h].begin(), chain[h].end());
    *chainCount = static_cast<uint32_t>(heads.size());
    return out;
}

}  // namespace

PlacementStats planFunctionPlacement(il::Module& mod, const spec::FeedbackPlan* plan) {
    PlacementStats stats;
    mod.hotFunctions.clear();
    mod.coldFunctions.clear();
    const uint32_t n = static_cast<uint32_t>(mod.functions.size());
    std::vector<uint64_t> bytes(n, 0);
    for (uint32_t f = 0; f < n; ++f) {
        for (const il::Block& b : mod.functions[f].blocks) bytes[f] += b.instructions.size();
        stats.instructions += bytes[f];
        bytes[f] *= kBytesPerInstruction;
    }

    // How often the run was found in each function: a harvest's samples, or
    // a recorded profile's entry counts. A stale harvest planned nothing.
    std::vector<uint64_t> heat(n, 0);
    uint64_t totalHeat = 0;
    if (plan != nullptr) {
        for (uint32_t f = 0; f < n; ++f) {
            const il::Function& fn = mod.functions[f];
            if (fn.isExternal || fn.blocks.empty()) continue;
            const auto s = plan->samples.find(fn.name);
            if (s != plan->samples.end()) heat[f] += s->second;
            const auto e = plan->entries.find(fn.name);
            if (e != plan->entries.end()) heat[f] += e->second;
            totalHeat += heat[f];
        }
    }
    stats.profiled = totalHeat != 0;
    // No profile (none, stale, or a run too short to be sampled): no
    // placement; the text is in module order, as it always was. Moving only
    // the stubs and the top level measured worse as often as better.
    if (!stats.profiled) return stats;

    // The hot set: the most sampled functions, to kHotCoverPermille.
    std::vector<uint32_t> ranked;
    for (uint32_t f = 0; f < n; ++f) {
        if (heat[f] != 0) ranked.push_back(f);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [&](uint32_t a, uint32_t b) { return heat[a] > heat[b]; });
    std::vector<uint8_t> hot(n, 0);
    uint64_t covered = 0;
    for (uint32_t f : ranked) {
        if (covered * 1000 >= totalHeat * kHotCoverPermille) break;
        hot[f] = 1;
        covered += heat[f];
    }

    // The calls out of a function: the callees the run's method sites named
    // (by their resolved names), and the direct calls of the IL, each with a
    // weight (a call in a loop counts kLoopWeight times).
    std::unordered_map<uint32_t, std::vector<uint32_t>> calleesByIc;
    for (const auto& [ic, names] : plan->methodsByIc) {
        for (const auto& [name, count] : names.counts) {
            const auto it = plan->callees.find(name);
            if (it != plan->callees.end() && it->second < n && count != 0) calleesByIc[ic].push_back(it->second);
        }
    }
    std::vector<uint32_t> targets;
    const auto forEachCall = [&](uint32_t f, const auto& visit) {
        const il::Function& fn = mod.functions[f];
        const std::vector<uint8_t> loops = loopBlocks(fn);
        for (il::BlockId b = 0; b < fn.blocks.size(); ++b) {
            const uint64_t w = loops[b] ? kLoopWeight : 1;
            for (const il::Instruction& inst : fn.blocks[b].instructions) {
                targets.clear();
                directCallees(inst, targets);
                if (isIcSite(inst)) {
                    const auto it = calleesByIc.find(inst.icIndex);
                    if (it != calleesByIc.end()) targets.insert(targets.end(), it->second.begin(), it->second.end());
                }
                for (uint32_t g : targets) {
                    if (g < n && g != f) visit(g, w);
                }
            }
        }
    };

    // What ran: a function the samples saw, one holding a site the run's
    // caches filled, and a callee a filled method site named.
    std::unordered_set<uint32_t> filled;
    for (const auto* by : {&plan->shapesByIc, &plan->methodShapesByIc, &plan->methodsByIc}) {
        for (const auto& [ic, names] : *by) {
            if (names.total != 0) filled.insert(ic);
        }
    }
    std::vector<uint8_t> ran(n, 0), speaks(n, 0);
    for (uint32_t f = 0; f < n; ++f) {
        const il::Function& fn = mod.functions[f];
        if (fn.isExternal) continue;
        ran[f] = heat[f] != 0 ? 1 : 0;
        for (const il::Block& b : fn.blocks) {
            for (const il::Instruction& inst : b.instructions) {
                if (!isIcSite(inst)) continue;
                speaks[f] |= siteSpeaks(inst) ? 1 : 0;
                if (filled.count(inst.icIndex) == 0) continue;
                ran[f] = 1;
                const auto it = calleesByIc.find(inst.icIndex);
                if (it == calleesByIc.end()) continue;
                for (uint32_t g : it->second) ran[g] = 1;
            }
        }
    }

    // A function that ran and that a hot one calls is hot too. A sample
    // credits the code before a poll, so a short callee with no loop that
    // runs between two polls of its caller is seen only when a sample falls
    // on its own entry, which the samples can miss.
    std::vector<uint32_t> hotList;
    for (uint32_t f = 0; f < n; ++f) {
        if (hot[f]) hotList.push_back(f);
    }
    // Only the sampled functions' own callees: what those call in turn the
    // samples would have seen if it mattered, and the closure of what ran
    // is most of the program again.
    std::vector<uint32_t> promoted;
    for (uint32_t f : hotList) {
        forEachCall(f, [&](uint32_t g, uint64_t) {
            if (hot[g] || !ran[g] || mod.functions[g].isExternal) return;
            hot[g] = 1;
            promoted.push_back(g);
        });
    }
    std::sort(promoted.begin(), promoted.end());
    std::vector<uint8_t> promotedSet(n, 0);
    for (uint32_t g : promoted) promotedSet[g] = 1;

    // The call edges among the sampled ones, for the chains; the promoted
    // follow them, in module order: they ran, the samples cannot say how
    // much, and joined to the chains they would spread the sampled code.
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint64_t>> callers;
    for (uint32_t f : hotList) {
        forEachCall(f, [&](uint32_t g, uint64_t w) {
            if (hot[g] && !promotedSet[g]) callers[g][f] += w;
        });
    }
    mod.hotFunctions = callChainOrder(hotList, heat, bytes, callers, &stats.chains);
    mod.hotFunctions.insert(mod.hotFunctions.end(), promoted.begin(), promoted.end());
    stats.hot = static_cast<uint32_t>(mod.hotFunctions.size());
    for (uint32_t f : mod.hotFunctions) stats.hotInstructions += bytes[f] / kBytesPerInstruction;

    // Cold: top-level code, and the functions whose inline-cache sites could
    // have filled and none did (they never ran).
    for (uint32_t f = 0; f < n; ++f) {
        const il::Function& fn = mod.functions[f];
        if (hot[f] || fn.isExternal) continue;
        if (isTopLevel(fn) || (speaks[f] && !ran[f])) mod.coldFunctions.push_back(f);
    }
    stats.cold = static_cast<uint32_t>(mod.coldFunctions.size());
    return stats;
}

std::string placementReport(const PlacementStats& s) {
    const auto n = [](uint64_t v) { return std::to_string(v); };
    if (!s.profiled) return "placement: no profile; module order\n";
    return "placement: hot " + n(s.hot) + " functions in " + n(s.chains) + " chains, " + n(s.hotInstructions) +
           " of " + n(s.instructions) + " instructions; cold " + n(s.cold) + "\n";
}

}  // namespace bronze::lower
