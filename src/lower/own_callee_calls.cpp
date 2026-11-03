// Own-callee direct calls (own_callee_calls.h).
//
// A method call whose receiver holds its callee in an own slot — the methods
// of a factory's returned literal (`WebGLState`'s `setFunc`, `WebGLProperties`'
// `get`), a callback stored by a constructor — goes through the method-call
// cache to the closure and then through the closure's generic entry. The whole
// program says which function that closure is; what it cannot fix is the
// closure object, since every factory call makes fresh ones. So the call is
// split on the closure's CODE, the way guarded inlining enters an own
// callee's body (inline_guarded.cpp, "own callees"):
//
//     f  = method.slot.fn recv, <site>       the slot way 0 of the cache names
//     ok = closure.is f, @T
//     br ok, E, S
//   E:
//     env = closure.env f                    (when T closed over a record)
//     r1  = call @T(env, recv, args...)      the direct entry
//     jump C(r1)
//   S:
//     r2  = method.call recv, ...            the original call
//     jump C(r2)
//   C(r):
//     the rest of the block
//
// `method.slot.fn` answers `undefined` when way 0 is not a slot entry for the
// receiver's shape, which `closure.is` refuses, so the original call stays the
// answer for every receiver the prediction did not describe. The direct entry
// is the one a closure's code word enters: the callee's parameters are its
// environment, its `this` and its declared parameters, and the call passes the
// first of the site's arguments, `undefined` for the missing ones, and drops
// the rest, which only `arguments` or a rest parameter could see.

#include "lower/own_callee_calls.h"

#include <utility>

namespace bronze::lower {

namespace {

constexpr size_t kMaxArgs = 16;

// Whether a call may enter `fn` directly with the operands this pass builds.
bool enterable(const il::Function& fn) {
    if (fn.isExternal || fn.blocks.empty() || fn.isCoroutineBody() || fn.isGenerator) return false;
    if (fn.needsArguments || fn.hasRestParam) return false;
    if (!il::functionEntersDirectly(fn)) return false;
    for (const auto& p : fn.params) {
        if (p.type != il::Type::Dynamic) return false;
    }
    return fn.returnType == il::Type::Dynamic || fn.returnType == il::Type::Void;
}

class Rewriter {
public:
    Rewriter(il::Module& module, const OwnCallees& callees) : module_(module), callees_(callees) {}

    uint32_t run() {
        uint32_t count = 0;
        for (il::Function& fn : module_.functions) {
            if (fn.isExternal || fn.blocks.empty()) continue;
            types_.assign(fn.valueCount, il::Type::Void);
            for (size_t p = 0; p < fn.params.size() && p < types_.size(); ++p) types_[p] = fn.params[p].type;
            for (const il::Block& b : fn.blocks) {
                for (const il::BlockParam& p : b.params) {
                    if (p.id < types_.size()) types_[p.id] = p.type;
                }
                for (const il::Instruction& inst : b.instructions) {
                    if (inst.result != il::kNoValue && inst.result < types_.size()) types_[inst.result] = inst.type;
                }
            }
            // The blocks this pass appends hold the original call (S), which is
            // not taken again, and the rest of a block (C), which is.
            const size_t original = fn.blocks.size();
            std::vector<uint8_t> done(original, 0);
            for (size_t b = 0; b < fn.blocks.size(); ++b) {
                if (b < done.size() && done[b]) continue;
                for (size_t i = 0; i < fn.blocks[b].instructions.size(); ++i) {
                    const uint32_t target = targetOf(fn, fn.blocks[b].instructions[i]);
                    if (target == kNone) continue;
                    split(fn, static_cast<il::BlockId>(b), i, target);
                    done.resize(fn.blocks.size(), 0);
                    done[fn.blocks.size() - 2] = 1;  // S
                    ++count;
                    break;
                }
            }
        }
        return count;
    }

private:
    static constexpr uint32_t kNone = UINT32_MAX;

    uint32_t targetOf(const il::Function& fn, const il::Instruction& inst) const {
        if (inst.op != il::Op::MethodCall || inst.operands.empty()) return kNone;
        if (inst.directTarget != il::Instruction::kNoDirectTarget) return kNone;
        const auto it = callees_.find(inst.icIndex);
        if (it == callees_.end() || it->second >= module_.functions.size()) return kNone;
        if (inst.operands.size() - 1 > kMaxArgs) return kNone;
        if (inst.result != il::kNoValue && inst.type != il::Type::Dynamic) return kNone;
        for (il::ValueId v : inst.operands) {
            if (v >= types_.size() || types_[v] != il::Type::Dynamic) return kNone;
        }
        if (&module_.functions[it->second] == &fn) return kNone;
        return enterable(module_.functions[it->second]) ? it->second : kNone;
    }

    il::ValueId fresh(il::Function& fn, il::Type t) {
        types_.push_back(t);
        return fn.valueCount++;
    }

    void split(il::Function& fn, il::BlockId b, size_t i, uint32_t target) {
        const il::Instruction call = fn.blocks[b].instructions[i];
        const il::Function& callee = module_.functions[target];
        const auto newBlock = [&]() -> il::BlockId {
            il::Block blk;
            blk.id = static_cast<il::BlockId>(fn.blocks.size());
            blk.handler = fn.blocks[b].handler;
            blk.copyClass = fn.blocks[b].copyClass;
            blk.copyRegion = fn.blocks[b].copyRegion;
            fn.blocks.push_back(std::move(blk));
            return fn.blocks.back().id;
        };
        const auto at = [&](il::Op op, il::Type type) {
            il::Instruction inst;
            inst.op = op;
            inst.type = type;
            inst.span = call.span;
            inst.inlineFrame = call.inlineFrame;
            return inst;
        };

        const il::BlockId enter = newBlock();
        const il::BlockId slow = newBlock();
        const il::BlockId tail = newBlock();
        {
            std::vector<il::Instruction> rest(fn.blocks[b].instructions.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                                              fn.blocks[b].instructions.end());
            fn.blocks[b].instructions.resize(i);
            fn.blocks[tail].instructions = std::move(rest);
            if (call.result != il::kNoValue) fn.blocks[tail].params.push_back({call.result, il::Type::Dynamic});
        }

        il::Instruction slotFn = at(il::Op::MethodSlotFn, il::Type::Dynamic);
        slotFn.result = fresh(fn, il::Type::Dynamic);
        slotFn.operands = {call.operands[0]};
        slotFn.icIndex = call.icIndex;
        slotFn.keyIndex = call.keyIndex;
        il::Instruction is = at(il::Op::ClosureIs, il::Type::Bool);
        is.result = fresh(fn, il::Type::Bool);
        is.operands = {slotFn.result};
        is.calleeIndex = target;
        il::Instruction br = at(il::Op::Branch, il::Type::Void);
        br.operands = {is.result};
        br.target.block = enter;
        br.elseTarget.block = slow;
        const il::ValueId slotValue = slotFn.result;
        fn.blocks[b].instructions.push_back(std::move(slotFn));
        fn.blocks[b].instructions.push_back(std::move(is));
        fn.blocks[b].instructions.push_back(std::move(br));

        // E: the direct entry, its operands the callee's parameters.
        il::Block& e = fn.blocks[enter];
        std::vector<il::ValueId> operands;
        if (callee.needsEnv) {
            il::Instruction env = at(il::Op::ClosureEnv, il::Type::Dynamic);
            env.result = fresh(fn, il::Type::Dynamic);
            env.operands = {slotValue};
            operands.push_back(env.result);
            e.instructions.push_back(std::move(env));
        }
        const size_t firstSource = callee.firstSourceParam();
        if (callee.needsThis) operands.push_back(call.operands[0]);
        il::ValueId undef = il::kNoValue;
        for (size_t p = firstSource; p < callee.params.size(); ++p) {
            const size_t arg = 1 + (p - firstSource);
            if (arg < call.operands.size()) {
                operands.push_back(call.operands[arg]);
                continue;
            }
            if (undef == il::kNoValue) {
                il::Instruction u = at(il::Op::ConstUndefined, il::Type::Dynamic);
                u.result = undef = fresh(fn, il::Type::Dynamic);
                e.instructions.push_back(std::move(u));
            }
            operands.push_back(undef);
        }
        il::Instruction direct = at(il::Op::Call, il::Type::Dynamic);
        direct.calleeIndex = target;
        direct.operands = std::move(operands);
        if (callee.needsEnv) direct.callEnvHops = 0;
        il::Instruction toTail = at(il::Op::Jump, il::Type::Void);
        toTail.target.block = tail;
        // A callee that returns nothing has no result to hand over: the
        // call's value is `undefined`.
        const bool voidCallee = callee.returnType == il::Type::Void;
        if (voidCallee) direct.type = il::Type::Void;
        il::ValueId answer = il::kNoValue;
        if (call.result != il::kNoValue && !voidCallee) {
            answer = direct.result = fresh(fn, il::Type::Dynamic);
        }
        e.instructions.push_back(std::move(direct));
        if (call.result != il::kNoValue && voidCallee) {
            il::Instruction u = at(il::Op::ConstUndefined, il::Type::Dynamic);
            u.result = answer = fresh(fn, il::Type::Dynamic);
            e.instructions.push_back(std::move(u));
        }
        if (answer != il::kNoValue) toTail.target.args.push_back(answer);
        e.instructions.push_back(std::move(toTail));

        // S: the original call, its result renamed.
        il::Instruction again = call;
        il::Instruction join = at(il::Op::Jump, il::Type::Void);
        join.target.block = tail;
        if (call.result != il::kNoValue) {
            again.result = fresh(fn, il::Type::Dynamic);
            join.target.args.push_back(again.result);
        }
        fn.blocks[slow].instructions.push_back(std::move(again));
        fn.blocks[slow].instructions.push_back(std::move(join));
    }

    il::Module& module_;
    const OwnCallees& callees_;
    std::vector<il::Type> types_;
};

}  // namespace

uint32_t applyOwnCalleeCalls(il::Module& module, const OwnCallees& callees) {
    if (callees.empty()) return 0;
    return Rewriter(module, callees).run();
}

}  // namespace bronze::lower
