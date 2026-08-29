// READ RUNS: the one rule that moves a guard point LATER.
//
// `guard_region.h` states what this is for and what licenses it. This file is
// the decision alone — which stretch of a block is a run of constant-index reads
// off one receiver, and which of that block's coalesced guard points therefore
// collapse into one. It writes `plan.splitsOf` and `plan.runGuardsOf` and
// nothing else; `guard_region_build.cpp` performs what it decided.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "il/key.h"
#include "lower/guard_region.h"

namespace bronze::lower {

namespace {

// A read this rule can reason about: `prop.get %recv, "<index>"` with the key a
// canonical array index, spelled through the SAME parser the backend's run
// planner uses (il/key.h says why that has to be one function).
//
// A site carrying a STATIC SLOT claim is not one. That claim puts its own guard
// in front of the cache (codegen-llvm/llvm_static_slot.h) and the backend's
// run-arm planner refuses such a member outright, so merging a point past it
// would spend an `is.dense_array` on a run that cannot become a group — and
// worse, the slot guard's miss arm is a cache access like any other, so the
// read is not the load the merge is licensed by.
bool indexReadOf(const il::Instruction& inst, const std::vector<std::string>& keys,
                 il::ValueId receiver, uint32_t& index) {
    if (inst.op != il::Op::PropGet || inst.result == il::kNoValue) return false;
    if (inst.operands.empty() || inst.operands[0] != receiver) return false;
    if (inst.staticSlot != il::Instruction::kNoStaticSlot) return false;
    if (inst.keyIndex >= keys.size()) return false;
    const std::optional<uint32_t> parsed = il::parseIndexKey(keys[inst.keyIndex]);
    if (!parsed) return false;
    index = *parsed;
    return true;
}

// Whatever a read is on, when the run has not chosen a receiver yet.
il::ValueId indexReadReceiver(const il::Instruction& inst, const std::vector<std::string>& keys) {
    if (inst.op != il::Op::PropGet || inst.result == il::kNoValue) return il::kNoValue;
    if (inst.operands.empty()) return il::kNoValue;
    if (inst.staticSlot != il::Instruction::kNoStaticSlot) return il::kNoValue;
    if (inst.keyIndex >= keys.size()) return il::kNoValue;
    return il::parseIndexKey(keys[inst.keyIndex]) ? inst.operands[0] : il::kNoValue;
}

// May this instruction stand between a run's first read and the merged guard?
//
// The question is about the FAST COPY, which is not the IL in front of us: the
// two shapes that dominate a matrix kernel are rewritten there rather than
// copied. A checked coercion of a guarded candidate is DELETED — its result
// becomes the raw unbox the guard emitted — and promotable arithmetic over
// candidates becomes `f64` arithmetic. Neither survives into the fast copy as
// something that can run user code, so neither ends a run. Everything else is
// judged as it stands, by the two effect predicates and nothing finer: an op
// added tomorrow answers yes to them and ends a run here without anybody having
// to remember this function.
bool pureInFastCopy(const il::Instruction& inst, const std::vector<il::Type>& types,
                    const std::vector<uint8_t>& isCandidate) {
    if (il::isTerminator(inst.op)) return false;
    if (isCheckedUnboxOf(inst, types) && inst.operands[0] < isCandidate.size() &&
        isCandidate[inst.operands[0]] != 0) {
        return true;
    }
    if (isPromotableArith(inst, types) && inst.result < isCandidate.size() &&
        isCandidate[inst.result] != 0) {
        return true;
    }
    return !il::canCollect(inst) && !il::canThrow(inst);
}

// One run found in a block: reads at `first .. lastRead` off `receiver`, and the
// largest index any of them takes.
struct ReadRun {
    uint32_t first = 0;
    uint32_t lastRead = 0;
    // One past the whole stretch that is reads-or-pure, which is where the
    // value tests go. It is past `lastRead` and not at it because the LAST
    // read's own guard point is at its first promoted use — one or two pure
    // instructions further on — and a chain that stopped short of it would
    // leave that read a point of its own and the merge one short.
    uint32_t limit = 0;
    il::ValueId receiver = il::kNoValue;
    uint32_t maxIndex = 0;
    uint32_t reads = 0;
};

// The runs of one block, left to right and non-overlapping. A run is opened by
// an index read and extended while every instruction is either another read off
// the same receiver or pure in the fast copy; anything else closes it. A
// redefinition of the receiver closes it too — the reads after that point are
// off a different object and one `is.dense_array` cannot answer for both.
std::vector<ReadRun> findReadRuns(const il::Block& block, const std::vector<std::string>& keys,
                                  const std::vector<il::Type>& types,
                                  const std::vector<uint8_t>& isCandidate) {
    std::vector<ReadRun> runs;
    const size_t n = block.instructions.size();
    size_t i = 0;
    while (i < n) {
        const il::ValueId recv = indexReadReceiver(block.instructions[i], keys);
        // The receiver has to be a boxed value for `is.dense_array` to test, and
        // it must not be one of the values this pass is promoting to `f64` —
        // the test reads a tagged value's bits and a promoted candidate has none
        // in the fast copy.
        if (recv == il::kNoValue || recv >= types.size() || types[recv] != il::Type::Dynamic ||
            (recv < isCandidate.size() && isCandidate[recv] != 0)) {
            ++i;
            continue;
        }
        ReadRun run;
        run.first = static_cast<uint32_t>(i);
        run.lastRead = static_cast<uint32_t>(i);
        run.receiver = recv;
        size_t j = i;
        for (; j < n; ++j) {
            const il::Instruction& inst = block.instructions[j];
            uint32_t index = 0;
            if (indexReadOf(inst, keys, recv, index)) {
                run.maxIndex = std::max(run.maxIndex, index);
                run.lastRead = static_cast<uint32_t>(j);
                ++run.reads;
                continue;
            }
            if (inst.result == recv) break;
            if (!pureInFastCopy(inst, types, isCandidate)) break;
        }
        run.limit = static_cast<uint32_t>(j);
        if (run.reads >= 2) {
            runs.push_back(run);
            i = run.lastRead + 1;
        } else {
            i = static_cast<size_t>(run.first) + 1;
        }
    }
    return runs;
}

}  // namespace

bool regionRunGuardsDisabled() {
    static const bool off = [] {
        const char* env = std::getenv("BRONZE_NO_REGION_RUN_GUARDS");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return off;
}

uint32_t mergeReadRunGuards(const il::Function& fn, const std::vector<std::string>& keys,
                            const std::vector<il::Type>& types, il::BlockId block,
                            std::vector<Candidate>& candidates, RegionPlan& plan) {
    std::vector<uint32_t>& points = plan.splitsOf[block];
    std::vector<RegionPlan::RunGuard>& guards = plan.runGuardsOf[block];
    guards.assign(points.size(), RegionPlan::RunGuard{});
    if (regionRunGuardsDisabled() || points.size() < 2) return 0;

    const il::Block& src = fn.blocks[block];
    const std::vector<ReadRun> runs = findReadRuns(src, keys, types, plan.isCandidate);
    if (runs.empty()) return 0;

    std::vector<uint32_t> kept;
    std::vector<RegionPlan::RunGuard> keptGuards;
    // The point each old point was moved to, so the candidates can follow.
    std::vector<std::pair<uint32_t, uint32_t>> moved;
    uint32_t removed = 0;

    size_t p = 0;
    for (const ReadRun& run : runs) {
        // The chain goes ONE PAST the last read: every candidate the run
        // produced is defined by then, and no use of the last one has been
        // emitted yet. The scan stopped at the first instruction the fast copy
        // does not make pure, and a terminator is one of those, so this index is
        // always inside the block; the bound is a belt on that.
        const uint32_t guardIndex = run.limit;
        if (guardIndex <= run.first || guardIndex >= src.instructions.size()) continue;

        // Points before the run keep what they had.
        while (p < points.size() && points[p] < run.first) {
            kept.push_back(points[p]);
            keptGuards.push_back(RegionPlan::RunGuard{});
            ++p;
        }
        const size_t firstInRun = p;
        while (p < points.size() && points[p] <= guardIndex) ++p;
        const size_t count = p - firstInRun;
        // One point inside a run is one point either way, and merging it would
        // pay an `is.dense_array` for nothing.
        if (count < 2) {
            for (size_t k = firstInRun; k < p; ++k) {
                kept.push_back(points[k]);
                keptGuards.push_back(RegionPlan::RunGuard{});
            }
            continue;
        }
        for (size_t k = firstInRun; k < p; ++k) moved.push_back({points[k], run.first});
        removed += static_cast<uint32_t>(count - 1);
        kept.push_back(run.first);
        keptGuards.push_back(RegionPlan::RunGuard{run.receiver, run.maxIndex, guardIndex});
    }
    while (p < points.size()) {
        kept.push_back(points[p]);
        keptGuards.push_back(RegionPlan::RunGuard{});
        ++p;
    }
    if (removed == 0) return 0;

    for (Candidate& cand : candidates) {
        if (cand.kind != CandidateKind::UseGuard || cand.guardBlock != block) continue;
        for (const auto& [from, to] : moved) {
            if (cand.guardIndex == from) cand.guardIndex = to;
        }
    }
    points = std::move(kept);
    guards = std::move(keptGuards);
    return removed;
}

}  // namespace bronze::lower
