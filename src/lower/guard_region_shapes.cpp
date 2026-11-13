// SHAPE GUARDS: which points test which receivers' shapes, and which sites the
// fast copy turns into bare loads and stores behind them.
//
// `guard_region.h` ("SHAPE GUARDS") states the rule. This file is the decision
// alone: it writes `plan.shapeTestsOf`, `plan.shapeGuardedOf` and the shape
// points it adds to `plan.splitsOf` / `plan.runGuardsOf`, and
// `guard_region_build.cpp` performs it.

#include <algorithm>
#include <string>
#include <vector>

#include "lower/guard_region.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

using ShapeTest = RegionPlan::ShapeTest;

// A site a point can cover: a static-slot read, or a static-slot write whose
// value the fast copy holds as a `box.f64` (a promoted value or a box), which
// owes no write barrier and which every slot representation but an object
// slot's takes (so never under a key one may be). An accessor
// site or a transition run's member is no slot access at all. `numberBox` marks
// the values a `box.f64` defines, which are Number boxes in both copies whether
// or not the region carries them as doubles.
bool coverableSite(const il::Instruction& inst, const std::vector<il::Type>& types, const RegionPlan& plan,
                   const std::vector<const Candidate*>& candidateOf, const std::vector<uint8_t>& numberBox) {
    if (inst.staticSlot == il::Instruction::kNoStaticSlot) return false;
    if (inst.directTarget != il::Instruction::kNoDirectTarget) return false;
    if (inst.operands.empty()) return false;
    const il::ValueId recv = inst.operands[0];
    if (recv >= types.size() || types[recv] != il::Type::Dynamic) return false;
    if (recv < plan.isCandidate.size() && plan.isCandidate[recv] != 0) return false;
    // A claim the recorded run saw miss keeps its miss in line.
    if (plan.missedClaims != nullptr && plan.missedClaims->count(inst.icIndex) != 0) return false;
    if (inst.op == il::Op::PropGet) return inst.result != il::kNoValue;
    if (inst.op != il::Op::PropSet || il::isDefineWrite(inst) || inst.operands.size() < 2) return false;
    if (inst.transitionRunCell != il::Instruction::kNoTransitionRun) return false;
    // A key whose slot may be an object one (il.h `slotReprObjectFields`)
    // takes no Number.
    if (plan.objectKeys != nullptr &&
        std::find(plan.objectKeys->begin(), plan.objectKeys->end(), inst.keyIndex) != plan.objectKeys->end()) {
        return false;
    }
    const il::ValueId val = inst.operands[1];
    if (val < numberBox.size() && numberBox[val] != 0) return true;
    const Candidate* c = val < candidateOf.size() ? candidateOf[val] : nullptr;
    return c != nullptr && (c->kind == CandidateKind::Promoted || c->kind == CandidateKind::BoxElide);
}

// Any static site at all, coverable or not: one that is not covered is an
// ordinary access, whose miss can run user code.
bool staticSite(const il::Instruction& inst) {
    return (inst.op == il::Op::PropGet || inst.op == il::Op::PropSet) &&
           inst.staticSlot != il::Instruction::kNoStaticSlot;
}

ShapeTest testOf(const il::Instruction& inst) {
    ShapeTest t;
    t.receiver = inst.operands[0];
    t.slot = inst.staticSlot;
    if (inst.familyLo != il::Instruction::kNoFamily) {
        t.familyLo = inst.familyLo;
        t.familySpan = inst.familySpan;
    } else {
        t.cell = inst.staticCellIndex;
    }
    return t;
}

// Two tests ask the same question when they name one receiver and one claim:
// one cell, or one family range (whose stamp answers for every slot of it).
bool sameQuestion(const ShapeTest& a, const ShapeTest& b) {
    if (a.receiver != b.receiver) return false;
    if (a.familyLo != il::Instruction::kNoFamily || b.familyLo != il::Instruction::kNoFamily) {
        return a.familyLo == b.familyLo && a.familySpan == b.familySpan;
    }
    return a.cell == b.cell;
}

void addTest(std::vector<ShapeTest>& tests, const ShapeTest& t) {
    for (const ShapeTest& have : tests) {
        if (sameQuestion(have, t)) return;
    }
    tests.push_back(t);
}

struct BlockShapes {
    const il::Function& fn;
    const std::vector<il::Type>& types;
    const std::vector<DefSite>& defs;
    const std::vector<const Candidate*>& candidateOf;
    const std::vector<uint8_t>& numberBox;
    RegionPlan& plan;
    il::BlockId b;

    // Inside a run's span (after its first read, up to its chain) a new split
    // would cut the run the point at `first` stands for.
    bool insideRun(uint32_t index) const {
        const auto& points = plan.splitsOf[b];
        const auto& guards = plan.runGuardsOf[b];
        for (size_t j = 0; j < points.size() && j < guards.size(); ++j) {
            if (guards[j].isRun() && index > points[j] && index <= guards[j].guardIndex) return true;
        }
        return false;
    }

    // An index read an `is.dense_array` licensed: a load off an Array in bounds.
    bool licensedIndexRead(const il::Instruction& inst, uint32_t index) const {
        if (inst.op != il::Op::PropGet || inst.operands.empty()) return false;
        const auto& points = plan.splitsOf[b];
        const auto& guards = plan.runGuardsOf[b];
        for (size_t j = 0; j < points.size() && j < guards.size(); ++j) {
            const RegionPlan::RunGuard& run = guards[j];
            if (run.receiver == il::kNoValue || index < points[j] || index >= run.guardIndex) continue;
            if (inst.operands[0] == run.receiver && inst.staticSlot == il::Instruction::kNoStaticSlot) return true;
        }
        return false;
    }

    // What leaves every shape as it was in the fast copy, a covered site aside.
    bool movesNoShape(const il::Instruction& inst, uint32_t index) const {
        return pureInFastCopy(inst, types, plan.isCandidate) || licensedIndexRead(inst, index);
    }

    // Whether `recv` exists at a split in front of instruction `index`.
    bool availableAt(il::ValueId recv, uint32_t index) const {
        if (recv >= defs.size()) return false;
        const DefSite& d = defs[recv];
        if (d.block != b) return true;
        return d.index == UINT32_MAX || d.index < index;
    }

    // What a shape point in front of `from` would buy: the sites of the
    // stretch from there to the first instruction that moves a shape, on
    // receivers that exist at the point, less the tests it would ask. A test is
    // as dear as the guard of the one site it answers, so a point pays only
    // where one test answers several sites (a family range, a cell two reads
    // share) — the split and its trampoline are the rest of its price.
    int32_t savedFrom(uint32_t from) const {
        const il::Block& block = fn.blocks[b];
        uint32_t sites = 0;
        std::vector<ShapeTest> tests;
        for (uint32_t i = from; i < block.instructions.size(); ++i) {
            const il::Instruction& inst = block.instructions[i];
            if (inst.result != il::kNoValue && inst.result < plan.aliasOf.size() &&
                plan.aliasOf[inst.result] != il::kNoValue) {
                continue;
            }
            if (staticSite(inst)) {
                if (!coverableSite(inst, types, plan, candidateOf, numberBox) || !availableAt(inst.operands[0], from)) break;
                ++sites;
                addTest(tests, testOf(inst));
                continue;
            }
            if (!movesNoShape(inst, i)) break;
        }
        return static_cast<int32_t>(sites) - static_cast<int32_t>(tests.size());
    }

    void insertPoint(size_t at, uint32_t index) {
        plan.splitsOf[b].insert(plan.splitsOf[b].begin() + static_cast<ptrdiff_t>(at), index);
        plan.runGuardsOf[b].insert(plan.runGuardsOf[b].begin() + static_cast<ptrdiff_t>(at), RegionPlan::RunGuard{});
        plan.shapeTestsOf[b].insert(plan.shapeTestsOf[b].begin() + static_cast<ptrdiff_t>(at),
                                    std::vector<ShapeTest>{});
    }

    void run() {
        const il::Block& block = fn.blocks[b];
        auto& raw = plan.shapeGuardedOf[b];
        raw.assign(block.instructions.size(), 0);
        plan.runGuardsOf[b].resize(plan.splitsOf[b].size());
        plan.shapeTestsOf[b].assign(plan.splitsOf[b].size(), {});

        // Receivers covered in the current stretch, with the point that tests
        // them; and the first instruction of the stretch.
        std::vector<std::pair<il::ValueId, size_t>> active;
        uint32_t stretchStart = 0;
        for (uint32_t i = 0; i < block.instructions.size(); ++i) {
            const il::Instruction& inst = block.instructions[i];
            // A repeated run read is not in the fast copy at all.
            if (inst.result != il::kNoValue && inst.result < plan.aliasOf.size() &&
                plan.aliasOf[inst.result] != il::kNoValue) {
                continue;
            }
            if (!staticSite(inst)) {
                if (!movesNoShape(inst, i)) {
                    active.clear();
                    stretchStart = i + 1;
                }
                continue;
            }
            const bool coverable = coverableSite(inst, types, plan, candidateOf, numberBox);
            const il::ValueId recv = inst.operands[0];
            size_t point = SIZE_MAX;
            for (const auto& [r, p] : active) {
                if (r == recv) point = p;
            }
            auto& points = plan.splitsOf[b];
            if (coverable && point == SIZE_MAX) {
                // The latest point of the stretch the receiver exists at.
                for (size_t j = 0; j < points.size(); ++j) {
                    if (points[j] < stretchStart || points[j] > i) continue;
                    if (availableAt(recv, points[j])) point = j;
                }
                if (point == SIZE_MAX && !insideRun(i) && savedFrom(i) >= (plan.missedClaims != nullptr ? 0 : 1)) {
                    size_t at = 0;
                    while (at < points.size() && points[at] < i) ++at;
                    if (at == points.size() || points[at] != i) insertPoint(at, i);
                    point = at;
                    for (auto& entry : active) {
                        if (entry.second >= at) ++entry.second;
                    }
                }
                if (point != SIZE_MAX) active.push_back({recv, point});
            }
            if (!coverable || point == SIZE_MAX) {
                active.clear();
                stretchStart = i + 1;
                continue;
            }
            addTest(plan.shapeTestsOf[b][point], testOf(inst));
            raw[i] = 1;
            ++plan.shapeGuardedCount;
        }
    }
};

}  // namespace

bool planShapeGuards(const il::Function& fn, const std::vector<il::Type>& types, RegionPlan& plan) {
    const std::vector<DefSite> defs = computeDefSites(fn);
    std::vector<const Candidate*> candidateOf(fn.valueCount, nullptr);
    for (const Candidate& c : plan.candidates) {
        if (c.value < candidateOf.size()) candidateOf[c.value] = &c;
    }
    std::vector<uint8_t> numberBox(fn.valueCount, 0);
    for (const il::Block& block : fn.blocks) {
        for (const il::Instruction& inst : block.instructions) {
            if (inst.op == il::Op::Box && inst.boxType == il::Type::F64 && inst.result < numberBox.size()) {
                numberBox[inst.result] = 1;
            }
        }
    }
    plan.shapeTestsOf.assign(fn.blocks.size(), {});
    plan.shapeGuardedOf.assign(fn.blocks.size(), {});
    plan.runGuardsOf.resize(fn.blocks.size());
    plan.shapeGuardedCount = 0;
    for (il::BlockId b : plan.blocks) {
        BlockShapes shapes{fn, types, defs, candidateOf, numberBox, plan, b};
        shapes.run();
        // A shape run's reads are loads only behind its point's tests, and its
        // value tests send a failure back in front of them all: one read left
        // an ordinary access would run a second time in the slow copy.
        const auto& points = plan.splitsOf[b];
        const auto& guards = plan.runGuardsOf[b];
        for (size_t j = 0; j < points.size(); ++j) {
            if (!guards[j].shapeRun) continue;
            for (uint32_t i = points[j]; i < guards[j].guardIndex; ++i) {
                const il::Instruction& inst = fn.blocks[b].instructions[i];
                const bool folded = inst.result != il::kNoValue && inst.result < plan.aliasOf.size() &&
                                    plan.aliasOf[inst.result] != il::kNoValue;
                if (staticSite(inst) && !folded && plan.shapeGuardedOf[b][i] == 0) return false;
            }
        }
    }
    return true;
}

}  // namespace bronze::lower
