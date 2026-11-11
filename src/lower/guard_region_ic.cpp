// IC EXITS: which inlined bodies' guards leave the fast copy on a miss.
//
// `guard_region.h` ("IC EXITS") states the rule. This file is the decision
// alone: it writes `plan.icExitOf` and the points it adds to `plan.splitsOf`
// (with `plan.runGuardsOf` and `plan.shapeTestsOf` kept parallel), and
// `guard_region_build.cpp` performs it.

#include <algorithm>
#include <vector>

#include "lower/guard_region.h"

namespace bronze::lower {

namespace {

// The ways into each block: its predecessors' edges, and one for each block
// that names it as its handler.
std::vector<uint32_t> entryCounts(const il::Function& fn) {
    std::vector<uint32_t> entries(fn.blocks.size(), 0);
    for (const il::Block& block : fn.blocks) {
        if (block.handler != il::kNoBlock && block.handler < entries.size()) ++entries[block.handler];
        if (block.instructions.empty()) continue;
        const il::Instruction& term = block.instructions.back();
        for (const il::BlockTarget* t : {&term.target, &term.elseTarget}) {
            if (t->block != il::kNoBlock && t->block < entries.size()) ++entries[t->block];
        }
    }
    return entries;
}

// Whether block `b` ends in the guard of an inlined body whose miss is the
// original site: `g = *.ic.is recv` and `br g, body, miss`, with `miss` a
// region block only this branch enters and whose first instruction is the
// call or access the guard stands for, on the same receiver. The backend
// reads the same shape to find a guard's miss (il_shape_memo.cpp,
// `find_guard_misses`).
bool icExitSite(const il::Function& fn, const std::vector<uint32_t>& entries, const RegionPlan& plan, il::BlockId b) {
    const std::vector<il::Instruction>& insts = fn.blocks[b].instructions;
    if (insts.size() < 2) return false;
    const il::Instruction& term = insts.back();
    const il::Instruction& guard = insts[insts.size() - 2];
    if (guard.op != il::Op::MethodIcIs && guard.op != il::Op::AccessorIcIs) return false;
    if (term.op != il::Op::Branch || term.operands.size() != 1 || guard.operands.empty() ||
        guard.result == il::kNoValue || term.operands[0] != guard.result) {
        return false;
    }
    const il::BlockId miss = term.elseTarget.block;
    if (miss == il::kNoBlock || miss >= fn.blocks.size() || miss == term.target.block || !plan.inRegion[miss]) {
        return false;
    }
    if (entries[miss] != 1 || fn.blocks[miss].instructions.empty()) return false;
    const il::Instruction& site = fn.blocks[miss].instructions.front();
    if (site.operands.empty() || site.operands[0] != guard.operands[0]) return false;
    if (guard.op == il::Op::MethodIcIs) return site.op == il::Op::MethodCall && site.icIndex == guard.icIndex;
    return (site.op == il::Op::PropGet || site.op == il::Op::PropSet) &&
           site.directTarget != il::Instruction::kNoDirectTarget && site.staticCellIndex == guard.staticCellIndex;
}

// Whether a `+`-chain accumulator is open in front of instruction `index`: a
// split there would cut a run that has to stay in one block.
bool concatOpenAt(const il::Block& block, uint32_t index) {
    int depth = 0;
    for (uint32_t i = 0; i < index; ++i) {
        if (block.instructions[i].op == il::Op::ConcatBegin) ++depth;
        if (block.instructions[i].op == il::Op::ConcatEnd) --depth;
    }
    return depth > 0;
}

}  // namespace

uint32_t countIcExitSites(const il::Function& fn, const RegionPlan& plan) {
    const std::vector<uint32_t> entries = entryCounts(fn);
    uint32_t sites = 0;
    for (il::BlockId b : plan.blocks) {
        if (icExitSite(fn, entries, plan, b)) ++sites;
    }
    return sites;
}

void planIcExits(const il::Function& fn, const std::function<bool(il::BlockId)>& served, RegionPlan& plan) {
    const std::vector<uint32_t> entries = entryCounts(fn);
    plan.icExitOf.assign(fn.blocks.size(), 0);
    plan.icExitCount = 0;
    for (il::BlockId b : plan.blocks) {
        if (!icExitSite(fn, entries, plan, b)) continue;
        const il::Block& block = fn.blocks[b];
        const uint32_t index = static_cast<uint32_t>(block.instructions.size()) - 2;
        std::vector<uint32_t>& points = plan.splitsOf[b];
        std::vector<RegionPlan::RunGuard>& runs = plan.runGuardsOf[b];
        // The exit's point is its own. A point already at the guard, or a run
        // whose span the guard stands in, keeps the block as it is.
        bool taken = false;
        for (size_t j = 0; j < points.size(); ++j) {
            if (points[j] == index) taken = true;
            if (j < runs.size() && runs[j].isRun() && index > points[j] && index <= runs[j].guardIndex) taken = true;
        }
        if (taken || concatOpenAt(block, index) || !served(b)) continue;
        const auto at = static_cast<std::ptrdiff_t>(std::lower_bound(points.begin(), points.end(), index) -
                                                    points.begin());
        points.insert(points.begin() + at, index);
        runs.insert(runs.begin() + at, RegionPlan::RunGuard{});
        plan.shapeTestsOf[b].insert(plan.shapeTestsOf[b].begin() + at, std::vector<RegionPlan::ShapeTest>{});
        plan.icExitOf[b] = 1;
        ++plan.icExitCount;
    }
}

}  // namespace bronze::lower
