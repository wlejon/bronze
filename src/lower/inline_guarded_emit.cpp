// Guarded method inlining: CFG/SSA splice emission (`Inliner::inlineAt`) and
// post-inline caller folding (`foldInlinedCallers`). Target eligibility and
// caller budget planning live in `inline_guarded.cpp`.

#include "lower/inline_guarded_internal.h"

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

#include "lower/guard_region_cfg.h"
#include "lower/typed_entry.h"

namespace bronze::lower {

namespace inline_guarded_detail {

bool trace() {
    static const bool on = std::getenv("BRONZE_INLINE_GUARDED_TRACE") != nullptr;
    return on;
}

size_t instructionCount(const il::Function& fn) {
    size_t n = 0;
    for (const auto& b : fn.blocks) n += b.instructions.size();
    return n;
}

std::vector<il::Type> valueTypes(const il::Function& fn) {
    std::vector<il::Type> t(fn.valueCount, il::Type::Void);
    for (size_t i = 0; i < fn.params.size() && i < t.size(); ++i) t[i] = fn.params[i].type;
    for (const auto& b : fn.blocks) {
        for (const auto& p : b.params) {
            if (p.id < t.size()) t[p.id] = p.type;
        }
        for (const auto& inst : b.instructions) {
            if (inst.result != il::kNoValue && inst.result < t.size()) t[inst.result] = inst.type;
        }
    }
    return t;
}

std::vector<uint8_t> loopBlocks(const il::Function& fn) {
    std::vector<uint8_t> in(fn.blocks.size(), 0);
    const Cfg cfg = buildCfg(fn);
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (cfg.rpoIndex[b] == UINT32_MAX) continue;
        for (il::BlockId h : cfg.succs[b]) {
            if (!cfg.dominates(h, static_cast<il::BlockId>(b))) continue;
            for (il::BlockId m : naturalLoop(cfg, h, static_cast<il::BlockId>(b))) in[m] = 1;
        }
    }
    return in;
}

// The closures this function makes, by value: the function, and the
// environment it closed over (kNoValue for a `func.ref`).
void Inliner::noteLiteral(const il::Instruction& inst) {
    if (inst.result == il::kNoValue) return;
    if (inst.op == il::Op::CreateFunction) {
        literals_[inst.result] = {inst.calleeIndex, inst.operands.empty() ? il::kNoValue : inst.operands[0]};
    } else if (inst.op == il::Op::FunctionRef) {
        literals_[inst.result] = {inst.calleeIndex, il::kNoValue};
    }
}

uint32_t Inliner::originOf(uint32_t f) const {
    const uint32_t o = module_.functions[f].typedOrigin;
    return o == il::Function::kNoTypedOrigin ? f : o;
}

il::ValueId Inliner::fresh(il::Function& fn, il::Type type) {
    const il::ValueId v = fn.valueCount++;
    types_.push_back(type);
    return v;
}

size_t Inliner::growthOf(uint32_t callee, size_t extra) const {
    return originals_[callee].blocks.size() * 2 + instructionCount(originals_[callee]) + 4 + extra;
}

void Inliner::inlineAt(il::Function& fn, il::BlockId b, size_t i, uint32_t calleeIdx, const SplicePlan& splice) {
    const Form form = splice.form;
    const bool closure = form == Form::Closure;
    const bool unguarded = form == Form::Exact || form == Form::Literal;
    const bool reuseSlotFn = closure && i == 0 && b < closureSlotFn_.size() && closureSlotFn_[b] != il::kNoValue;
    const il::ValueId priorSlotFn = reuseSlotFn ? closureSlotFn_[b] : il::kNoValue;
    const il::Function& callee = originals_[calleeIdx];
    const il::Instruction call = fn.blocks[b].instructions[i];
    const il::BlockId handler = fn.blocks[b].handler;
    const il::CopyClass copyClass = fn.blocks[b].copyClass;
    const uint32_t copyRegion = fn.blocks[b].copyRegion;

    // The frame every copied instruction names.
    // Frames and the guard name the generic function: its descriptor is
    // the one a real call's frame reports, and its wrapper is the code
    // word the site's cache latches (a typed entry is reached only behind
    // that same test).
    const uint32_t origin = closure || form == Form::Literal                        ? calleeIdx
                            : call.directTarget != il::Instruction::kNoDirectTarget ? call.directTarget
                                                                                    : originOf(calleeIdx);
    fn.inlineFrames.push_back(il::Function::InlineFrame{origin, call.span, call.inlineFrame});
    const uint32_t frame = static_cast<uint32_t>(fn.inlineFrames.size());

    const auto newBlock = [&]() -> il::BlockId {
        il::Block blk;
        blk.id = static_cast<il::BlockId>(fn.blocks.size());
        blk.handler = handler;
        blk.copyClass = copyClass;
        blk.copyRegion = copyRegion;
        fn.blocks.push_back(std::move(blk));
        slowBlock_.push_back(0);
        closureTried_.push_back(0);
        methodTried_.push_back(0);
        closureSlotFn_.push_back(il::kNoValue);
        reservedForBlock_.push_back(0);
        inLoop_.push_back(inLoop_[b]);
        return fn.blocks.back().id;
    };
    const auto callSiteInst = [&](il::Op op) {
        il::Instruction inst;
        inst.op = op;
        inst.span = call.span;
        inst.inlineFrame = call.inlineFrame;
        return inst;
    };

    // C: the rest of B, entered with the call's result.
    const il::BlockId tail = newBlock();
    {
        std::vector<il::Instruction> rest(fn.blocks[b].instructions.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                                          fn.blocks[b].instructions.end());
        fn.blocks[b].instructions.resize(i);
        fn.blocks[tail].instructions = std::move(rest);
        if (call.result != il::kNoValue) fn.blocks[tail].params.push_back({call.result, il::Type::Dynamic});
    }

    // S: the original call, unchanged but for the id its result takes.
    // When `moreTries` holds, S stays open to the next chained closure or
    // method target (and, for a static closure guess, to any prototype target);
    // an unguarded body has none.
    const il::BlockId slow = unguarded ? il::kNoBlock : newBlock();
    if (!unguarded) {
        slowBlock_[slow] = splice.moreTries ? 0 : 1;
        if (closure) {
            closureTried_[slow] = splice.nextTry;
        } else if (splice.moreTries) {
            methodTried_[slow] = splice.nextTry;
        }
        reservedForBlock_[slow] = splice.reserveSlow;
    }
    if (!unguarded) {
        il::Instruction again = call;
        il::Instruction jump = callSiteInst(il::Op::Jump);
        jump.target.block = tail;
        if (call.result != il::kNoValue) {
            again.result = fresh(fn, il::Type::Dynamic);
            jump.target.args.push_back(again.result);
        }
        fn.blocks[slow].instructions.push_back(std::move(again));
        fn.blocks[slow].instructions.push_back(std::move(jump));
    }

    // E: a closure body's entry, where its environment is read.
    const il::BlockId envBlock = closure ? newBlock() : il::kNoBlock;

    // The callee's blocks, numbered after S, and every value it defines.
    const il::BlockId base = static_cast<il::BlockId>(fn.blocks.size());
    for (size_t k = 0; k < callee.blocks.size(); ++k) newBlock();
    // A copied block in one of the callee's loops is in a loop here: a
    // builtin's body brings its walk, and the callback's call in it is
    // the call the walk makes per element.
    {
        const std::vector<uint8_t> calleeLoops = loopBlocks(callee);
        for (size_t k = 0; k < callee.blocks.size(); ++k) inLoop_[base + k] |= calleeLoops[k];
    }
    // The callee's guarded regions keep their fast/slow identity under
    // fresh region numbers, so their copies still overlay each other's
    // frame slots. Inside one of the caller's own copies, every block is
    // that copy: a block cannot name two regions, and the outer pairing is
    // the one that is true of all of them.
    if (copyClass == il::CopyClass::Shared) {
        uint32_t regions = 0;
        for (const auto& blk : fn.blocks) {
            if (blk.copyRegion != il::kNoCopyRegion) regions = std::max(regions, blk.copyRegion + 1);
        }
        for (size_t k = 0; k < callee.blocks.size(); ++k) {
            const il::Block& cb = callee.blocks[k];
            if (cb.copyRegion == il::kNoCopyRegion) continue;
            fn.blocks[base + k].copyClass = cb.copyClass;
            fn.blocks[base + k].copyRegion = regions + cb.copyRegion;
        }
    }
    std::vector<il::ValueId> vm(callee.valueCount, il::kNoValue);

    // Parameters: `this`, then the source parameters in order; one the
    // call does not pass is `undefined`, as the callee's frame holds it.
    il::ValueId undef = il::kNoValue;
    const auto undefinedAtHead = [&]() {
        if (undef == il::kNoValue) {
            il::Instruction u = callSiteInst(il::Op::ConstUndefined);
            u.type = il::Type::Dynamic;
            u.result = undef = fresh(fn, il::Type::Dynamic);
            fn.blocks[b].instructions.push_back(std::move(u));
        }
        return undef;
    };
    const size_t firstSource = callee.firstSourceParam();
    il::ValueId envValue = il::kNoValue;
    if (form == Form::Exact) {
        // A direct call's operands are its callee's parameters.
        for (size_t p = 0; p < callee.params.size(); ++p) vm[p] = call.operands[p];
    } else {
        // A call.dynamic's callee and `this` come before its arguments.
        const size_t self = form == Form::Literal ? 1 : 0;
        if (callee.needsThis) vm[firstSource - 1] = call.operands[self];
        if (closure && callee.needsEnv) vm[0] = envValue = fresh(fn, il::Type::Dynamic);
        if (form == Form::Literal && callee.needsEnv) vm[0] = literals_.at(call.operands[0]).second;
        for (size_t p = firstSource; p < callee.params.size(); ++p) {
            const size_t arg = self + 1 + (p - firstSource);
            vm[p] = arg < call.operands.size() ? call.operands[arg] : undefinedAtHead();
        }
    }
    for (const auto& cb : callee.blocks) {
        for (const auto& prm : cb.params) vm[prm.id] = fresh(fn, prm.type);
        for (const auto& inst : cb.instructions) {
            if (inst.result != il::kNoValue) vm[inst.result] = fresh(fn, inst.type);
        }
    }
    const auto map = [&](il::ValueId v) { return v < vm.size() ? vm[v] : il::kNoValue; };
    const auto mapTarget = [&](il::BlockTarget t) {
        if (t.block != il::kNoBlock) t.block = base + t.block;
        for (auto& a : t.args) a = map(a);
        return t;
    };

    // B ends in the guard: the method cache's, or an accessor site's cell,
    // or for a closure guess the slot's closure's code word; an unguarded
    // body is entered outright.
    if (unguarded) {
        il::Instruction enter = callSiteInst(il::Op::Jump);
        enter.target.block = base;
        fn.blocks[b].instructions.push_back(std::move(enter));
    } else if (closure) {
        il::ValueId slotFnValue = priorSlotFn;
        if (slotFnValue == il::kNoValue) {
            il::Instruction slotFn = callSiteInst(il::Op::MethodSlotFn);
            slotFn.type = il::Type::Dynamic;
            slotFn.result = slotFnValue = fresh(fn, il::Type::Dynamic);
            slotFn.operands = {call.operands[0]};
            slotFn.icIndex = call.icIndex;
            slotFn.keyIndex = call.keyIndex;
            fn.blocks[b].instructions.push_back(std::move(slotFn));
        }
        if (splice.moreTries) closureSlotFn_[slow] = slotFnValue;
        il::Instruction is = callSiteInst(il::Op::ClosureIs);
        is.type = il::Type::Bool;
        is.result = fresh(fn, il::Type::Bool);
        is.operands = {slotFnValue};
        is.calleeIndex = calleeIdx;
        il::Instruction br = callSiteInst(il::Op::Branch);
        br.operands = {is.result};
        br.target.block = envBlock;
        br.elseTarget.block = slow;
        if (envValue != il::kNoValue) {
            il::Instruction env = callSiteInst(il::Op::ClosureEnv);
            env.type = il::Type::Dynamic;
            env.result = envValue;
            env.operands = {slotFnValue};
            fn.blocks[envBlock].instructions.push_back(std::move(env));
        }
        il::Instruction enter = callSiteInst(il::Op::Jump);
        enter.target.block = base;
        fn.blocks[envBlock].instructions.push_back(std::move(enter));
        fn.blocks[b].instructions.push_back(std::move(is));
        fn.blocks[b].instructions.push_back(std::move(br));
    } else {
        const bool accessor = call.op != il::Op::MethodCall;
        il::Instruction guard = callSiteInst(accessor ? il::Op::AccessorIcIs : il::Op::MethodIcIs);
        guard.type = il::Type::Bool;
        guard.result = fresh(fn, il::Type::Bool);
        guard.operands = {call.operands[0]};
        if (accessor) {
            guard.staticCellIndex = call.staticCellIndex;
        } else {
            guard.icIndex = call.icIndex;
        }
        guard.keyIndex = call.keyIndex;
        guard.calleeIndex = origin;
        il::Instruction br = callSiteInst(il::Op::Branch);
        br.operands = {guard.result};
        br.target.block = base;
        br.elseTarget.block = slow;
        fn.blocks[b].instructions.push_back(std::move(guard));
        fn.blocks[b].instructions.push_back(std::move(br));
    }

    for (size_t k = 0; k < callee.blocks.size(); ++k) {
        const il::Block& cb = callee.blocks[k];
        il::Block& out = fn.blocks[base + k];
        for (const auto& prm : cb.params) out.params.push_back({vm[prm.id], prm.type});
        for (const auto& src : cb.instructions) {
            if (src.op == il::Op::Ret) {
                il::Instruction jump = src;
                jump.op = il::Op::Jump;
                jump.type = il::Type::Void;
                jump.operands.clear();
                jump.inlineFrame = frame;
                jump.target = il::BlockTarget{tail, {}};
                jump.elseTarget = il::BlockTarget{};
                if (call.result != il::kNoValue) {
                    il::ValueId v = (!src.operands.empty() && src.operands[0] != il::kNoValue) ? map(src.operands[0])
                                                                                                : il::kNoValue;
                    if (v == il::kNoValue) {
                        il::Instruction u = src;
                        u.op = il::Op::ConstUndefined;
                        u.type = il::Type::Dynamic;
                        u.operands.clear();
                        u.inlineFrame = frame;
                        u.result = v = fresh(fn, il::Type::Dynamic);
                        out.instructions.push_back(std::move(u));
                    }
                    jump.target.args.push_back(v);
                }
                out.instructions.push_back(std::move(jump));
                continue;
            }
            il::Instruction inst = src;
            inst.inlineFrame = frame;
            if (inst.result != il::kNoValue) inst.result = vm[src.result];
            for (auto& o : inst.operands) o = map(o);
            noteLiteral(inst);
            if (inst.op == il::Op::Jump || inst.op == il::Op::Branch) {
                inst.target = mapTarget(src.target);
                if (inst.op == il::Op::Branch) inst.elseTarget = mapTarget(src.elseTarget);
            }
            out.instructions.push_back(std::move(inst));
        }
    }
}

}  // namespace inline_guarded_detail

void foldInlinedCallers(il::Module& module, const std::vector<uint32_t>& changed) {
    for (uint32_t f : changed) {
        il::Function folded = module.functions[f];
        if (foldTypedEntry(folded, &module) == 0) continue;
        // A forwarded block parameter can leave a value read in a block
        // numbered above its definition's; dominance itself still holds, so
        // the order is what is repaired.
        if (!dominanceHolds(folded)) {
            std::vector<uint8_t> built;
            orderAfterDominators(folded, built);
        }
        if (!dominanceHolds(folded)) continue;
        module.functions[f] = std::move(folded);
    }
}

}  // namespace bronze::lower
