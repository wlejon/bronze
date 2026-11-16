// Job 0: BOUNDS-CHECK WIDENING.
//
// A run of element accesses at `i`, `i + 1`, ... `i + k` (a vertex packed
// into a typed array, a matrix read out of one) makes a guard per access:
// `in.bounds (add i, c), n` and, for the int sum, `add.would_ovf i, c`. When
// every path from the first test that does not leave reaches the others, the
// first test is widened to cover them all: `in.bounds x, n, +span` with x the
// first index (i + c0) and span the largest offset beyond it, which holds when
// x >= 0 and x + span < n. Its true edge then proves base + c in [0, n) for
// every c in c0..c0+span (edge_facts.h InRange), which answers the later
// bounds tests and their overflow tests (tests.cpp), so job 1 folds them.
//
// Why exact: the widened test fails only when one of the tests it covers
// would fail on a path the function takes anyway (the chain below forces it,
// short of an earlier leave), and its exit is the first test's own exit: G
// resumes in front of the first access, before anything the later ones do,
// and runs all of them generically.
//
// The CHAIN from a guard: its passing successor, and from each block on it the
// one successor that is not a leave (a `jump`, or a `br` whose other arm is a
// block ending in `leave`). Any other terminator ends it. Only offsets at or
// above the first one's are taken, so the widened test's lower end is the
// first index itself.

#include <algorithm>
#include <cstdint>
#include <unordered_set>

#include "opt/core/edge_facts.h"
#include "opt/core/uses.h"
#include "opt/redundancy/jobs.h"

namespace bronze::opt::redundancy {

namespace {

using il::Op;

// How many blocks a chain is followed through.
constexpr size_t kMaxChainBlocks = 256;

bool endsInLeave(const il::Function& fn, BlockId b) {
    const auto& insts = fn.blocks[b].instructions;
    return !insts.empty() && insts.back().op == Op::Leave;
}

// The successor of `b` that every path through `b` not leaving takes, or
// kNoBlock.
BlockId forcedSuccessor(const il::Function& fn, BlockId b) {
    const auto& insts = fn.blocks[b].instructions;
    if (insts.empty()) return kNoBlock;
    const il::Instruction& term = insts.back();
    if (term.op == Op::Jump) return term.target.block;
    if (term.op != Op::Branch) return kNoBlock;
    const bool thenLeaves = endsInLeave(fn, term.target.block);
    const bool elseLeaves = endsInLeave(fn, term.elseTarget.block);
    if (thenLeaves == elseLeaves) return kNoBlock;
    return thenLeaves ? term.elseTarget.block : term.target.block;
}

}  // namespace

bool runBoundsWidening(JobContext& ctx) {
    il::Function& fn = ctx.fn;
    if (!fn.isSpeculated()) return false;
    const DefUse defs(fn);
    auto defOf = [&](ValueId v) { return defs.defInst(fn, v); };
    bool changed = false;
    for (BlockId b = 0; b < fn.blocks.size(); ++b) {
        auto& insts = fn.blocks[b].instructions;
        if (insts.size() < 2 || insts.back().op != Op::Branch) continue;
        // The guard: a branch on an in.bounds of this block, one arm leaving.
        const ValueId cond = insts.back().operands.empty() ? kNoValue : insts.back().operands[0];
        il::Instruction* head = nullptr;
        for (il::Instruction& inst : insts) {
            if (inst.op == Op::InBounds && inst.result == cond && inst.operands.size() == 2) head = &inst;
        }
        if (head == nullptr || forcedSuccessor(fn, b) == kNoBlock) continue;
        const auto [base, c0] = offsetForm(head->operands[0], defOf);
        const ValueId length = head->operands[1];
        int64_t hi = static_cast<int64_t>(c0) + head->immI32;

        std::unordered_set<BlockId> seen{b};
        BlockId at = forcedSuccessor(fn, b);
        for (size_t steps = 0; at != kNoBlock && steps < kMaxChainBlocks && seen.insert(at).second; ++steps) {
            for (const il::Instruction& inst : fn.blocks[at].instructions) {
                if (inst.op != Op::InBounds || inst.operands.size() != 2 || inst.operands[1] != length) continue;
                const auto [otherBase, c] = offsetForm(inst.operands[0], defOf);
                if (otherBase != base || c < c0) continue;
                hi = std::max(hi, static_cast<int64_t>(c) + inst.immI32);
            }
            at = forcedSuccessor(fn, at);
        }
        const int64_t span = hi - c0;
        if (span <= head->immI32 || hi > INT32_MAX) continue;
        head->immI32 = static_cast<int32_t>(span);
        changed = true;
    }
    return changed;
}

}  // namespace bronze::opt::redundancy
