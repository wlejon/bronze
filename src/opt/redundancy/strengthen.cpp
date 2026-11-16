// GUARD STRENGTHENING (speculated functions): a hole guard on an element that
// is then guarded as a Number becomes the Number guard.
//
//     x = elem.load a, i ; br is.hole x, E1, F      ; E1 leaves before the read
//     F: ... (guards and jumps) ...
//     br is.number x, G, E2
//
// becomes `br is.number x, F, E1`: the hole is no Number, so the stronger test
// still fails on it, and the later test is then answered by the facts and goes
// with its exit (value_walk). Leaving earlier than S would have is always
// exact (E1 hands G the state in front of the read, and G redoes what S had
// not done yet); what the rule must make sure of is only that S does not leave
// where it would have carried on, so the Number test has to be on the one
// path F starts: from F through jumps and guard branches (one side an exit)
// only, never past a branch both of whose sides continue in S.
//
// One test less per element read in matrix code (three.js's
// Matrix4.multiplyMatrices reads 32 elements and multiplies each).

#include <cstddef>

#include "opt/core/uses.h"
#include "opt/redundancy/jobs.h"

namespace bronze::opt::redundancy {

namespace {

using il::Op;

// A cold block that does exit work and leaves: the failing side of a guard.
bool isExit(const il::Function& fn, BlockId b) {
    const il::Block& block = fn.blocks[b];
    return block.cold && !block.instructions.empty() && block.instructions.back().op == Op::Leave;
}

// Whether `is.number x` is reached on every way on from `start` that stays in
// S, looking through at most a bounded stretch of code.
bool numberTestFollows(const il::Function& fn, BlockId start, ValueId x) {
    constexpr size_t kMaxInstructions = 4000;
    size_t seen = 0;
    BlockId cur = start;
    for (size_t steps = 0; steps < fn.blocks.size(); ++steps) {
        const il::Block& block = fn.blocks[cur];
        for (const il::Instruction& inst : block.instructions) {
            if (inst.op == Op::IsNumber && inst.operands.size() == 1 && inst.operands[0] == x) return true;
            if (++seen > kMaxInstructions) return false;
        }
        if (block.instructions.empty()) return false;
        const il::Instruction& term = block.instructions.back();
        if (term.op == Op::Jump) {
            cur = term.target.block;
        } else if (term.op == Op::Branch && isExit(fn, term.target.block) != isExit(fn, term.elseTarget.block)) {
            cur = isExit(fn, term.target.block) ? term.elseTarget.block : term.target.block;
        } else {
            return false;
        }
    }
    return false;
}

}  // namespace

bool runGuardStrengthening(JobContext& ctx) {
    il::Function& fn = ctx.fn;
    if (!fn.isSpeculated()) return false;
    bool changed = false;
    for (il::Block& block : fn.blocks) {
        if (block.instructions.size() < 2) continue;
        il::Instruction& term = block.instructions.back();
        if (term.op != Op::Branch || term.operands.size() != 1) continue;
        // `br is.hole x, exit, fast`, the test made in this block.
        il::Instruction* test = nullptr;
        for (il::Instruction& inst : block.instructions) {
            if (inst.result == term.operands[0]) test = &inst;
        }
        if (test == nullptr || test->op != Op::IsHole || test->operands.size() != 1) continue;
        if (!isExit(fn, term.target.block) || isExit(fn, term.elseTarget.block)) continue;
        if (!term.target.args.empty() || !term.elseTarget.args.empty()) continue;
        const ValueId x = test->operands[0];
        if (!numberTestFollows(fn, term.elseTarget.block, x)) continue;
        // A new test in front of the branch; the hole test, now unused here,
        // is dead code's to remove.
        il::Instruction number;
        number.op = Op::IsNumber;
        number.type = il::Type::Bool;
        number.operands = {x};
        number.result = newValue(fn);
        number.span = test->span;
        term.operands[0] = number.result;
        std::swap(term.target, term.elseTarget);
        block.instructions.insert(block.instructions.end() - 1, std::move(number));
        ++ctx.stats.guardsStrengthened;
        changed = true;
    }
    return changed;
}

}  // namespace bronze::opt::redundancy
