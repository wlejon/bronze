// Which callees and which call sites the inliner may take, and the points
// after the caller's generic calls it finishes them at.
//
// A copy of a body behaves as the call did exactly when the body reads
// nothing that belongs to its own activation other than its parameters, which
// the call's operands bind one for one. What follows lists what a body CAN
// read of its own activation and why each is refused or harmless:
//
//   - `__env`, `__this`, the source parameters: parameters, bound to the
//     call's operands. `this` is not coerced anywhere outside the body: the
//     call wrapper passes it as it came (il_lowering_wrapper.cpp), and a
//     direct call passes the receiver as it is (direct_call.cpp), so a sloppy
//     body's coercion, if any, is in its own IL and is copied with it.
//   - `arguments` (needsArguments): built by the wrapper from the count it
//     sees. Never a direct-call target anyway; refused.
//   - a rest parameter: an array the caller of a direct call builds, so the
//     binding would still be exact; refused all the same, since the
//     speculation step never makes such a call and a lowered one is rare.
//   - new.target and super() (get.new_target, call.super*): read the
//     runtime's construct scope, which only the runtime's call pushes;
//     functionEntersDirectly refuses them, with generators, async functions,
//     natives and coroutine bodies.
//   - coro.start / coro.suspend / coro.mode: a coroutine frame is the
//     function's own (and brass pairs each coro_create with its body over the
//     whole module); refused.
//   - a class constructor: calling one throws, so no direct call reaches one.
//   - strictness: the backend reads Function::isStrict only in the call
//     wrapper, for the `arguments` object (il_lowering_wrapper.cpp); every
//     strict-dependent instruction carries its mode in its own immediate
//     (prop.set, elem.set, super.set, prop.delete, ...). So a strict body
//     copied into a sloppy caller stays strict, and the reverse; nothing is
//     refused for it.
//   - import.meta: keyed by the module URL in the instruction; harmless.
//   - template sites, inline-cache indices and slow sites: per source site,
//     so the copy shares them with the callee, as the language wants (one
//     template object per site) and as the caches can bear.
//   - the top level and the entry point: run once; refused.
//
// A plain G (no speculated copy) is never named by a direct call when it has
// resume points: a G with resume points has an S, and every call of it was
// redirected to the S (opt/speculate driver.cpp redirectEntry).

#include <cassert>

#include "abi/bronze_abi.h"
#include "il/effects.h"
#include "opt/inline/parts.h"

namespace bronze::opt::inliner {

bool isExitBlock(const il::Block& block) {
    return !block.instructions.empty() && block.instructions.back().op == il::Op::Leave;
}

bool hasLeaves(const il::Function& fn) {
    for (const il::Block& block : fn.blocks) {
        if (isExitBlock(block)) return true;
    }
    return false;
}

uint32_t bodySize(const il::Function& fn) {
    uint32_t size = 0;
    for (const il::Block& block : fn.blocks) {
        if (!isExitBlock(block)) size += static_cast<uint32_t>(block.instructions.size());
    }
    return size;
}

bool calleeInlinable(const il::Function& fn) {
    if (fn.isExternal || fn.blocks.empty() || fn.isEntryPoint) return false;
    if ((fn.descFlags & BRONZE_FN_DESC_TOPLEVEL) != 0) return false;
    if (!il::functionEntersDirectly(fn)) return false;
    if ((fn.fnFlags & BRONZE_ABI_FN_FLAG_CLASS_CTOR) != 0) return false;
    if (fn.needsArguments || fn.hasRestParam) return false;
    for (const il::Block& block : fn.blocks) {
        for (const il::Instruction& inst : block.instructions) {
            switch (inst.op) {
                case il::Op::CoroStart:
                case il::Op::CoroSuspend:
                case il::Op::CoroMode:
                    return false;
                // See the head of the file: a direct call never names a G
                // with resume points.
                case il::Op::ResumePoint:
                    assert(false && "a direct call names a generic function with resume points");
                    return false;
                // Each `ret` becomes a jump passing its operand to the
                // continuation, whose parameter has the return type.
                case il::Op::Ret:
                    if (inst.operands.size() != (fn.returnType == il::Type::Void ? 0u : 1u)) return false;
                    break;
                // The one rule (inline.h) rewrites leaves of the callee's own
                // G; a speculated function has no other (verifier).
                case il::Op::Leave:
                    if (!fn.isSpeculated() || inst.calleeIndex != fn.speculatedFrom) {
                        assert(false && "a leave names a function other than its own G");
                        return false;
                    }
                    break;
                default:
                    break;
            }
        }
    }
    return true;
}

AfterCallPoints::AfterCallPoints(const il::Function& generic) {
    for (const il::Block& block : generic.blocks) {
        for (size_t i = 1; i < block.instructions.size(); ++i) {
            const il::Instruction& point = block.instructions[i];
            if (point.op != il::Op::ResumePoint || point.immI32 < 0) continue;
            const il::Instruction& call = block.instructions[i - 1];
            // Only the three forms a `resumeAfter` is placed behind
            // (method_call.cpp, plain_call.cpp, driver.cpp
            // markLoweredDirectCalls). A point behind anything else stands in
            // front of a site and is no after-call point.
            if (call.op != il::Op::DynamicCall && call.op != il::Op::MethodCall && call.op != il::Op::Call) {
                continue;
            }
            AfterCall after;
            after.resumeId = static_cast<uint32_t>(point.immI32);
            after.state = point.operands;
            for (ValueId v : point.operands) {
                if (call.result != kNoValue && v == call.result) {
                    after.callResult = v;
                    after.callResultType = call.type;
                }
            }
            points_.emplace(after.resumeId, std::move(after));
        }
    }
}

const AfterCall* AfterCallPoints::find(uint32_t id) const {
    const auto it = points_.find(id);
    return it == points_.end() ? nullptr : &it->second;
}

bool resultMakeable(il::Type inState, il::Type returned) {
    if (returned == il::Type::Ptr) return false;
    if (inState == returned) return returned != il::Type::Void;
    return inState == il::Type::Dynamic;
}

bool siteFits(const il::Instruction& call, const il::Function& callee, const AfterCall* after) {
    if (call.op != il::Op::Call) return false;
    // The operands ARE the parameter list (the verifier's direct-call rule),
    // which is what makes binding them one for one exact.
    if (call.operands.size() != callee.params.size()) return false;
    // The continuation's parameter takes each `ret`'s operand.
    if (call.type != callee.returnType) return false;
    if (call.type == il::Type::Void && call.result != kNoValue) return false;
    // Operand 0 is the callee's record itself only with no hops; with hops it
    // is an ancestor walk the IL has no instruction for (the backend calls
    // bronze_env_ancestor), so such a call stays a call.
    if (call.callEnvHops != il::Instruction::kNoEnvHops && call.callEnvHops != 0) return false;
    if (!hasLeaves(callee)) return true;
    // A guard of the copy needs somewhere to leave to in the caller's G: the
    // point after the generic call. None in a protected block (resume_rules.h),
    // so a call under a handler takes no callee with guards.
    if (after == nullptr) return false;
    return after->callResult == kNoValue || resultMakeable(after->callResultType, callee.returnType);
}

}  // namespace bronze::opt::inliner
