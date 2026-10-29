// NUMBER WINDOWS: which of a guard point's `is.number` tests an
// `is.number_array` claim can answer instead (guard_region.h, "NUMBER ARRAYS").
// This file is the decision alone; guard_region_build.cpp emits what it
// decided, and il2mir (il_number_array.cpp) lowers the claim.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "lower/guard_region.h"

namespace bronze::lower {

namespace {

// A claim costs a header test, two loads and a branch on the path where it
// holds; an `is.number` it answers costs a compare and a branch. Below this
// many answered tests per claim the window is not worth its claims.
constexpr size_t kSkipsPerClaim = 10;

// The guarded values a point's chain tests, ascending: the `UseGuard`
// candidates assigned to it.
std::vector<il::ValueId> guardedAt(const RegionPlan& plan, il::BlockId block, uint32_t index) {
    std::vector<il::ValueId> out;
    for (const Candidate& c : plan.candidates) {
        if (c.kind == CandidateKind::UseGuard && c.guardBlock == block && c.guardIndex == index) {
            out.push_back(c.value);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The window of one point: the longest stretch ending at `hi` (exclusive),
// read backwards, of constant-index reads off boxed receivers and instructions
// pure in the fast copy, trimmed to start at its first read. A receiver
// redefined inside the stretch ends it there, since a claim in front of the
// window could not name the value the later reads are off. It may reach back
// past earlier guard points: their chains are tests, which are pure too.
struct Window {
    uint32_t start = 0;
    std::vector<RegionPlan::NumberClaim> claims;
    std::vector<il::ValueId> skippable;  // ascending
};

Window findWindow(const il::Block& block, const std::vector<std::string>& keys, const std::vector<il::Type>& types,
                  const std::vector<uint8_t>& isCandidate, uint32_t hi, const std::vector<il::ValueId>& guarded) {
    Window window;
    window.start = hi;
    for (uint32_t i = hi; i > 0; --i) {
        const il::Instruction& inst = block.instructions[i - 1];
        bool redefines = false;
        for (const auto& claim : window.claims) redefines = redefines || inst.result == claim.receiver;
        if (redefines) break;
        const il::ValueId recv = indexReadReceiver(inst, keys);
        if (recv != il::kNoValue) {
            // The claim reads a tagged value's bits, so the receiver must be one
            // in the fast copy: boxed, and not a value this pass promotes.
            if (recv >= types.size() || types[recv] != il::Type::Dynamic ||
                (recv < isCandidate.size() && isCandidate[recv] != 0)) {
                break;
            }
            uint32_t index = 0;
            if (!indexReadOf(inst, keys, recv, index)) break;
            auto it = std::find_if(window.claims.begin(), window.claims.end(),
                                   [&](const RegionPlan::NumberClaim& c) { return c.receiver == recv; });
            if (it == window.claims.end()) {
                window.claims.push_back({recv, index});
            } else {
                it->maxIndex = std::max(it->maxIndex, index);
            }
            window.start = i - 1;
            continue;
        }
        if (!pureInFastCopy(inst, types, isCandidate)) break;
    }
    for (uint32_t i = window.start; i < hi; ++i) {
        const il::Instruction& inst = block.instructions[i];
        if (inst.result == il::kNoValue || indexReadReceiver(inst, keys) == il::kNoValue) continue;
        if (std::binary_search(guarded.begin(), guarded.end(), inst.result)) {
            window.skippable.push_back(inst.result);
        }
    }
    return window;
}

// Merges `extra` into `into`, by receiver, keeping the larger index.
void addClaims(std::vector<RegionPlan::NumberClaim>& into, const std::vector<RegionPlan::NumberClaim>& extra) {
    for (const auto& claim : extra) {
        auto it = std::find_if(into.begin(), into.end(),
                               [&](const RegionPlan::NumberClaim& c) { return c.receiver == claim.receiver; });
        if (it == into.end()) {
            into.push_back(claim);
        } else {
            it->maxIndex = std::max(it->maxIndex, claim.maxIndex);
        }
    }
}

}  // namespace

bool numberArraysDisabled() {
    static const bool off = [] {
        const char* env = std::getenv("BRONZE_NO_NUMBER_ARRAYS");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return off;
}

void planNumberWindows(const il::Function& fn, const std::vector<std::string>& keys,
                       const std::vector<il::Type>& types, RegionPlan& plan) {
    plan.numberWindowsOf.assign(fn.blocks.size(), {});
    for (il::BlockId b : plan.blocks) {
        const std::vector<uint32_t>& points = plan.splitsOf[b];
        std::vector<RegionPlan::NumberWindow>& out = plan.numberWindowsOf[b];
        out.assign(points.size(), RegionPlan::NumberWindow{});
        if (numberArraysDisabled()) continue;
        const il::Block& block = fn.blocks[b];

        // Groups of points sharing one set of claims, left to right: a point
        // joins the open group when its window reaches back to the group's
        // claims, and opens a new one otherwise.
        struct Group {
            uint32_t owner = 0;
            uint32_t claimAt = 0;
            std::vector<RegionPlan::NumberClaim> claims;
            std::vector<uint32_t> members;
            size_t skips = 0;
        };
        std::vector<Group> groups;
        std::vector<Window> windows(points.size());
        for (size_t j = 0; j < points.size(); ++j) {
            const RegionPlan::RunGuard run =
                j < plan.runGuardsOf[b].size() ? plan.runGuardsOf[b][j] : RegionPlan::RunGuard{};
            // An ordinary point's chain stands at its split, a run point's at
            // the end of its run (guard_region.h, READ RUNS).
            const uint32_t hi = run.receiver != il::kNoValue ? run.guardIndex : points[j];
            if (hi == 0 || hi > block.instructions.size()) continue;
            windows[j] = findWindow(block, keys, types, plan.isCandidate, hi, guardedAt(plan, b, points[j]));
            const Window& w = windows[j];
            if (w.skippable.empty()) continue;
            if (!groups.empty() && w.start <= groups.back().claimAt) {
                Group& g = groups.back();
                addClaims(g.claims, w.claims);
                g.members.push_back(static_cast<uint32_t>(j));
                g.skips += w.skippable.size();
            } else {
                Group g;
                g.owner = static_cast<uint32_t>(j);
                g.claimAt = w.start;
                g.claims = w.claims;
                g.members.push_back(static_cast<uint32_t>(j));
                g.skips = w.skippable.size();
                groups.push_back(std::move(g));
            }
        }
        for (const Group& g : groups) {
            if (g.skips < kSkipsPerClaim * g.claims.size()) continue;
            out[g.owner].claimAt = g.claimAt;
            out[g.owner].claims = g.claims;
            for (uint32_t j : g.members) {
                out[j].owner = g.owner;
                out[j].skippable = windows[j].skippable;
            }
        }
    }
}

}  // namespace bronze::lower
