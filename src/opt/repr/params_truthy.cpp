// Rule 4: a dynamic block parameter that is only ever TESTED becomes a Bool.
//
// `a || b` and `a && b` in a condition join the operand that decided the
// result with a boolean on the other edge, so the join's parameter is
// dynamic even though everything that reads it asks ToBoolean of it
// (`unbox.bool`) or passes it on to another such join:
//
//     b1:  ... br %c, b3(%x), b2          ; %x: whatever `a` was
//     b2:  %t = box.bool %e ; jump b3(%t)
//     b3(%p: dynamic): %q = unbox.bool %p ; br %q, ...
//
// A parameter p qualifies when every use of it is the operand of an
// `unbox.bool` or an argument passed to another qualifying parameter
// (candidates start as every dynamic block parameter and lose those with any
// other use, to a fixed point, so a chain of joins qualifies together). Then
// p becomes a Bool that holds ToBoolean of what it held, every `unbox.bool p`
// is p, and every edge passes the truth value of its argument: the operand of
// a `box.bool`, a qualifying parameter as it is, or a new `unbox.bool` of the
// argument in front of the source block's terminator. ToBoolean runs no user
// code and cannot throw (ECMA-262 7.1.2), so computing it on the edge rather
// than in the block changes nothing but where it runs, and the redundancy
// pass merges it with a test of the same value the source block already made
// (the `%c` above, usually).

#include <utility>
#include <vector>

#include "opt/core/uses.h"
#include "opt/repr/params.h"
#include "opt/repr/phases.h"
#include "opt/repr/rewrite.h"

namespace bronze::opt::repr {

using il::Op;
using il::Type;

namespace {

bool isTruthTest(const il::Instruction& inst) {
    return inst.op == Op::Unbox && inst.type == Type::Bool && inst.operands.size() == 1 &&
           inst.result != kNoValue;
}

}  // namespace

bool truthyBlockParams(PhaseContext& ctx) {
    il::Function& fn = ctx.fn;
    const DefUse defs(fn);
    // cand[v]: v is a dynamic block parameter still qualifying.
    std::vector<uint8_t> cand(defs.valueCount(), 0);
    bool anyCand = false;
    for (size_t b = 1; b < fn.blocks.size(); ++b) {
        for (const il::BlockParam& p : fn.blocks[b].params) {
            if (p.type == Type::Dynamic && p.id < cand.size()) {
                cand[p.id] = 1;
                anyCand = true;
            }
        }
    }
    if (!anyCand) return false;
    auto useQualifies = [&](ValueId v, const UseSite& use) {
        const il::Instruction& inst = fn.blocks[use.block].instructions[use.inst];
        if (use.kind == UseSite::Kind::Operand) return isTruthTest(inst) && inst.operands[0] == v;
        const BlockId to = use.kind == UseSite::Kind::ThenArg ? inst.target.block : inst.elseTarget.block;
        const auto& params = fn.blocks[to].params;
        return use.slot < params.size() && params[use.slot].id < cand.size() && cand[params[use.slot].id] != 0;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (ValueId v = 0; v < cand.size(); ++v) {
            if (cand[v] == 0) continue;
            for (const UseSite& use : defs.uses(v)) {
                if (!useQualifies(v, use)) {
                    cand[v] = 0;
                    changed = true;
                    break;
                }
            }
        }
    }

    // A parameter none of whose uses is a test (only passed around) gains
    // nothing on its own; it still qualifies, so a chain converts whole.
    bool changedFn = false;
    const std::vector<std::vector<InEdge>> inEdges = collectInEdges(fn);
    EditPlan edits;
    std::vector<ValueId> replace(defs.valueCount(), kNoValue);
    for (size_t b = 1; b < fn.blocks.size(); ++b) {
        auto& params = fn.blocks[b].params;
        for (size_t pi = 0; pi < params.size(); ++pi) {
            const ValueId p = params[pi].id;
            if (p >= cand.size() || cand[p] == 0) continue;
            for (const InEdge& e : inEdges[b]) {
                ValueId& arg = inEdgeArgs(fn, e)[pi];
                if (arg < cand.size() && cand[arg] != 0) continue;
                if (const std::optional<BoxSource> s = boxSource(fn, defs, arg); s && s->type == Type::Bool) {
                    arg = s->value;
                    continue;
                }
                il::Instruction test;
                test.op = Op::Unbox;
                test.type = Type::Bool;
                test.operands = {arg};
                test.result = newValue(fn);
                test.span = fn.blocks[e.from].instructions.back().span;
                arg = test.result;
                edits.insertBefore(e.from, fn.blocks[e.from].instructions.size() - 1, std::move(test));
            }
            for (const UseSite& use : defs.uses(p)) {
                if (use.kind != UseSite::Kind::Operand) continue;
                const il::Instruction& inst = fn.blocks[use.block].instructions[use.inst];
                replace[inst.result] = p;
                edits.remove(use.block, use.inst);
            }
            params[pi].type = Type::Bool;
            ++ctx.stats.paramsTruthy;
            changedFn = true;
        }
    }
    if (!changedFn) return false;
    edits.apply(fn);
    replaceAllUses(fn, replace);
    return true;
}

}  // namespace bronze::opt::repr
