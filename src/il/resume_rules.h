#pragma once

// Where the backend can resume a generic function: the rule a pass that
// places `resume.point` (il_op.h) asks before it places one, and the backend
// lowers by (il2mir/il_lowering_resume.cpp).
//
// A resume point of G becomes an entry of G's resume twin, which starts there
// from the point's state alone. So the place must be one the state can
// describe:
//   - not in a coroutine body, nor in a function that starts one: brass lowers
//     coroutines over the whole module once, and a suspension's frame is not
//     a state a twin can rebuild;
//   - not in the module's entry (the top level): it registers the module's
//     tables and runs once, and has no twin;
//   - not in a block a handler protects, nor in a handler block: the twin is
//     entered with no exception edge open, and a handler block reads the
//     exception its edge carries, which no state names;
//   - not inside a string concatenation chain (canResumeBefore).

#include "abi/bronze_abi.h"
#include "il/il.h"

namespace bronze::il {

// Whether a function may hold resume points at all.
inline bool canResumeIn(const Function& fn) {
    if (fn.isExternal || fn.isEntryPoint || fn.isCoroutineBody() || fn.isSpeculated()) return false;
    if ((fn.descFlags & BRONZE_FN_DESC_TOPLEVEL) != 0) return false;
    for (const Block& b : fn.blocks) {
        for (const Instruction& inst : b.instructions) {
            if (inst.op == Op::CoroStart || inst.op == Op::CoroSuspend) return false;
        }
    }
    return true;
}

// Whether a resume point may stand in front of instruction `index` of
// `block` (canResumeAt holding for the block): not inside a string
// concatenation chain, between a concat.begin and its concat.end. The
// accumulator live there is a builder its next link appends to in place,
// used exactly once (il/verifier.cpp); a point's state naming it would be a
// second use, and a twin resumed with it would append to a builder the
// generic code goes on to extend. Chains nest (an operand may hold one), so
// the depth is counted.
inline bool canResumeBefore(const Block& block, uint32_t index) {
    int open = 0;
    for (uint32_t i = 0; i < index && i < block.instructions.size(); ++i) {
        const Op op = block.instructions[i].op;
        if (op == Op::ConcatBegin) ++open;
        if (op == Op::ConcatEnd) --open;
    }
    return open == 0;
}

// Whether `block` of `fn` may hold a resume point (canResumeIn holding for
// `fn`).
inline bool canResumeAt(const Function& fn, const Block& block) {
    if (block.handler != kNoBlock) return false;
    for (const Block& b : fn.blocks) {
        if (b.handler == block.id) return false;
    }
    return true;
}

}  // namespace bronze::il
