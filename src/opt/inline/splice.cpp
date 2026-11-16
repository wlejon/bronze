// THE SPLICE: one direct call replaced by a copy of the callee's body.
//
//   before                          after
//   B:  ...                         B:  ...
//       r = call @F(a0, ..., an)        jump E'
//       rest...                     E': (F's entry, copied; F's params := a0..an)
//                                       ...
//                                   X': ret v        ->  jump T(v)
//                                   L': leave @G_f, id, s...
//                                         ->  t = resume.call @G_f, id, s...
//                                             leave @G_c, a, callerState(t)
//                                   T(r):  rest...
//
// Why each step is exact:
//   - B is split in front of the call (opt/core splitBlockBefore): the
//     instructions before it stay in B, which keeps every index in front of
//     the call and every other block's id, so the driver's remaining sites
//     (taken back to front in each block) are still where it found them.
//   - F's parameters are F's values 0..n-1 and are defined by no block, so
//     seeding the clone map with them maps every use to the call's operand
//     in the same position; every other value of F is defined in a copied
//     block and gets a fresh id (cloneBlocks).
//   - Every block of F is copied, reachable or not: a handler a non-throwing
//     block names, and a block after a `ret`, are still named by the copied
//     targets and handlers, and a block outside the set would keep its old id,
//     which in the caller names some other block. Unreachable copies are left
//     for the redundancy pass to delete.
//   - A copy with no handler (a throw in F leaves F) gets the call's block's
//     handler: a throw out of the call went there. Copies that had one keep
//     their copy of it (F's own try/catch).
//   - Each `ret` passes its operand to T, whose parameter IS the call's old
//     result id, so every use of the result in the caller is unchanged and
//     defined at T's top. B dominates every copy (E' is entered only from B;
//     F's other blocks only from E'), and the copies are T's only
//     predecessors, so B dominates T: the caller's values used in T and after
//     are still available, and so are those an exit's state names.
//   - A leave is rewritten by the one rule (inline.h). The caller's state is
//     its point `a` in G_c's ids, which S_c keeps (cloneFunction); the
//     generic call's result is replaced by what G_f returned, converted as
//     the fast path converts it (resultMakeable). Every other value of the
//     state was live after the generic call in G_c and was not its result,
//     so it is defined before the call: in S_c, before the direct call, on a
//     path B lies on (the speculation step re-defines every replaced result
//     under its old id on the fast path, guard_builder.h).

#include <cassert>
#include <numeric>
#include <utility>

#include "opt/core/cfg_edit.h"
#include "opt/core/clone.h"
#include "opt/core/uses.h"
#include "opt/inline/parts.h"

namespace bronze::opt::inliner {

namespace {

// The exit of a copied leave: `resume.call` of the callee's G, the conversion
// of its value to the generic call's result type when that result is in the
// state, and the caller's own leave after the call. `leave` is the copied
// leave, already removed from `blockId`.
uint32_t finishGenerically(il::Module& module, il::Function& caller, BlockId blockId, const il::Instruction& leave,
                           const AfterCall& after, ValueId instance) {
    const il::Function& calleeGeneric = module.functions[leave.calleeIndex];
    std::vector<il::Instruction>& insts = caller.blocks[blockId].instructions;
    uint32_t added = 0;

    il::Instruction rc;
    rc.op = il::Op::ResumeCall;
    rc.type = calleeGeneric.returnType;
    rc.calleeIndex = leave.calleeIndex;
    rc.immI32 = leave.immI32;
    rc.operands = leave.operands;
    rc.span = leave.span;
    if (rc.type != il::Type::Void) rc.result = newValue(caller);
    const ValueId returned = rc.result;
    insts.push_back(std::move(rc));
    ++added;

    // The value standing for the generic call's result, made only when the
    // state needs one (siteFits checked that it can be made).
    ValueId standIn = kNoValue;
    if (after.callResult != kNoValue) {
        assert(resultMakeable(after.callResultType, calleeGeneric.returnType));
        if (after.callResultType == calleeGeneric.returnType) {
            standIn = returned;
        } else {
            assert(after.callResultType == il::Type::Dynamic);
            il::Instruction conv;
            conv.result = newValue(caller);
            conv.type = il::Type::Dynamic;
            conv.span = leave.span;
            if (calleeGeneric.returnType == il::Type::Void) {
                conv.op = il::Op::ConstUndefined;
            } else {
                conv.op = il::Op::Box;
                conv.boxType = calleeGeneric.returnType;
                conv.operands = {returned};
            }
            standIn = conv.result;
            insts.push_back(std::move(conv));
            ++added;
        }
        if (after.construct) {
            assert(instance != kNoValue && after.callResultType == il::Type::Dynamic);
            il::Instruction rule;
            rule.op = il::Op::ConstructResult;
            rule.result = newValue(caller);
            rule.type = il::Type::Dynamic;
            rule.operands = {standIn, instance};
            rule.span = leave.span;
            standIn = rule.result;
            insts.push_back(std::move(rule));
            ++added;
        }
    }

    il::Instruction out;
    out.op = il::Op::Leave;
    out.type = il::Type::Void;
    out.calleeIndex = caller.speculatedFrom;
    out.immI32 = static_cast<int32_t>(after.resumeId);
    out.operands = after.state;
    for (ValueId& v : out.operands) {
        if (v == after.callResult) v = standIn;
    }
    out.span = leave.span;
    insts.push_back(std::move(out));
    return added;
}

}  // namespace

SpliceResult spliceCall(il::Module& module, uint32_t callerIndex, BlockId block, size_t index,
                        const il::Function& callee, const AfterCall* after) {
    il::Function& caller = module.functions[callerIndex];
    assert(caller.isSpeculated() && block < caller.blocks.size());
    assert(index + 1 < caller.blocks[block].instructions.size() && "a call is not a terminator");
    const il::Instruction call = caller.blocks[block].instructions[index];
    assert(siteFits(call, callee, after) && calleeInlinable(callee));
    assert(&callee != &caller && "a self-call splices a snapshot, not the function being edited");
    const BlockId siteHandler = caller.blocks[block].handler;
    const bool siteCold = caller.blocks[block].cold;
    SpliceResult result;

    // B | T: the call heads T, and comes out; its result becomes T's
    // parameter under the same id.
    const BlockId tail = splitBlockBefore(caller, block, index);
    {
        il::Block& t = caller.blocks[tail];
        assert(!t.instructions.empty() && t.instructions.front().op == il::Op::Call);
        t.instructions.erase(t.instructions.begin());
        if (call.result != kNoValue) t.params.push_back(il::BlockParam{call.result, call.type});
    }

    // The copy, F's parameters bound to the call's operands.
    CloneMap map;
    for (size_t p = 0; p < callee.params.size(); ++p) {
        map.mapValue(static_cast<ValueId>(p), call.operands[p]);
    }
    std::vector<BlockId> all(callee.blocks.size());
    std::iota(all.begin(), all.end(), BlockId{0});
    const auto first = static_cast<BlockId>(caller.blocks.size());
    cloneBlocks(callee, all, caller, map);
    const BlockId entry = map.block(0);
    assert(entry == first && caller.blocks[entry].params.empty() && "b0 takes no parameters");

    // The copy's positions are the callee's, in a scope of the caller's for
    // this call (il.h Function::inlineScopes); the callee's own scopes nest
    // under it.
    const auto scopeBase = static_cast<uint32_t>(caller.inlineScopes.size());
    const uint32_t ownScope = scopeBase + 1;
    caller.inlineScopes.push_back(il::Function::InlineScope{call.calleeIndex, call.span, call.inlineScope});
    for (const il::Function::InlineScope& inner : callee.inlineScopes) {
        caller.inlineScopes.push_back(
            il::Function::InlineScope{inner.callee, inner.call, inner.parent == 0 ? ownScope : scopeBase + 1 + inner.parent});
    }
    for (BlockId id = first; id < static_cast<BlockId>(caller.blocks.size()); ++id) {
        for (il::Instruction& inst : caller.blocks[id].instructions) {
            inst.inlineScope = inst.inlineScope == 0 ? ownScope : scopeBase + 1 + inst.inlineScope;
        }
    }

    // B enters the copy instead of T.
    {
        il::Instruction& jump = caller.blocks[block].instructions.back();
        assert(jump.op == il::Op::Jump && jump.target.block == tail && jump.target.args.empty());
        jump.target.block = entry;
    }

    // Returns and leaves of the copy. No block is added from here on, so the
    // ids stay put; instructions are only appended to the block being fixed.
    const auto end = static_cast<BlockId>(caller.blocks.size());
    for (BlockId id = first; id < end; ++id) {
        il::Block& copy = caller.blocks[id];
        if (copy.handler == kNoBlock) copy.handler = siteHandler;
        copy.cold = copy.cold || siteCold;
        result.instructionsAdded += static_cast<uint32_t>(copy.instructions.size());
        // A copied call's `resumeAfter` names a point of the callee's G, not
        // the caller's, and the pass never takes a copied call again: drop it
        // now, so the caller verifies after every splice.
        for (il::Instruction& inst : copy.instructions) inst.resumeAfter = il::Instruction::kNoResumeAfter;
        assert(!copy.instructions.empty() && il::isTerminator(copy.instructions.back().op));
        il::Instruction& term = copy.instructions.back();
        if (term.op == il::Op::Ret) {
            il::Instruction jump;
            jump.op = il::Op::Jump;
            jump.span = term.span;
            jump.target.block = tail;
            // A result nothing uses has no id and no parameter; the returned
            // value was computed anyway and is dropped, as the call dropped it.
            if (call.result != kNoValue) {
                assert(term.operands.size() == 1);
                jump.target.args = {term.operands[0]};
            }
            term = std::move(jump);
            continue;
        }
        if (term.op != il::Op::Leave) continue;
        // siteFits: a callee with leaves only at a call with its point after.
        assert(after != nullptr && term.calleeIndex == callee.speculatedFrom);
        assert(copy.handler == kNoBlock && "an exit block is never protected");
        const il::Instruction leave = std::move(term);
        copy.instructions.pop_back();
        // A speculated `new` passed its instance as `__this` (construct.cpp).
        const ValueId instance =
            callee.needsThis ? call.operands[callee.needsEnv ? 1 : 0] : kNoValue;
        const size_t kept = caller.blocks[id].instructions.size();
        result.instructionsAdded += finishGenerically(module, caller, id, leave, *after, instance);
        // The generic finish is the caller's call of the callee's G, made
        // where the call was: a trace through it shows G's own frame.
        for (size_t k = kept; k < caller.blocks[id].instructions.size(); ++k) {
            caller.blocks[id].instructions[k].inlineScope = call.inlineScope;
            caller.blocks[id].instructions[k].span = call.span;
        }
        ++result.leavesRewritten;
    }
    return result;
}

}  // namespace bronze::opt::inliner
