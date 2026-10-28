// Guarded method inlining: a resolved `method.call`'s callee body compiled at
// the call, behind the site's own method-cache test (docs/il2mir.md,
// "Guarded method inlining", states the design and why each part is exact).
//
// The shape of one inlined call, for `r = method.call recv, k, site, a...` in
// block B with callee T:
//
//     B:  ...head...                    C(r):  ...the rest of B...
//         g = method.ic.is recv, site, @T
//         br g, T'0(), S()              S:     r2 = method.call recv, k, site, a...
//                                              jump C(r2)
//     T'0 .. T'n: T's blocks, its parameters the call's operands, each `ret v`
//                 a `jump C(v)`, every instruction in inline frame k.
//
// `r` keeps its id as C's parameter, so no use after the call is rewritten:
// both ways into C supply it, and B dominates C as it dominated the rest of
// itself. Every new block takes B's handler, so a throw anywhere in the body
// or in the slow call leaves to wherever a throw out of the call would have.
//
// An ACCESSOR site (`prop.get` / `prop.set` naming its getter or setter, il.h
// `directTarget`) is inlined the same way, as a call of that body with the
// receiver as `this` and, for a setter, the stored value as its one argument.
// Its guard is `accessor.ic.is` on the site's cell pair, the test its own
// direct edge makes, and S keeps the original access, whose miss is what
// publishes the cell. A setter's return value is discarded, as [[Set]]
// discards it: a `prop.set` has no result for C to take.

#include "lower/inline_guarded.h"
#include "lower/guard_region_cfg.h"
#include "lower/typed_entry.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "il/print.h"
#include "il/verifier.h"

namespace bronze::lower {

namespace {

// The sizes below are in IL instructions of FINISHED bodies (guarded regions
// and typed entries already built), and were set on the three.js benches
// (docs/il2mir.md section 10 has the measurements).
//
// A callee's size: the body a site pays for in code and build time on top of
// the call it keeps for the slow path. `Matrix4.compose` (209) is inside it;
// leaving it out while its caller `Object3D.updateMatrix` came in measured
// slower than inlining neither, since the call left behind sat in a loop body
// that now held every other value live across it.
constexpr size_t kMaxCalleeInstructions = 250;
// Inline frames deep: a body inlined into a body inlined into the caller.
constexpr size_t kMaxDepth = 3;
// A caller bigger than this takes no bodies: what is big already is mostly
// cold library code (parsers, loaders), and brass's analyses grow faster than
// linearly in a function's size, so it is build time spent for nothing.
constexpr size_t kMaxCallerStart = 800;
// A caller grows to at most twice its size plus this, and never past the cap.
constexpr size_t kCallerGrowthSlack = 200;
constexpr size_t kMaxCallerInstructions = 1500;
// The fixed-argument method.call form's widest list (il/verifier.cpp).
constexpr size_t kMaxArgs = 16;

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

// Whether `fn`'s body means the same thing run at any method call that
// would have entered it: it reads nothing of the activation a real call
// builds beyond `this` and its parameters.
bool isCandidate(const il::Function& fn) {
    if (fn.isExternal || fn.blocks.empty() || fn.isCoroutineBody() || fn.isGenerator) return false;
    // The environment, `arguments` and the rest array are built by the call
    // wrapper from the real call; a sloppy function's `this` is coerced there.
    if (fn.needsEnv || fn.needsArguments || fn.hasRestParam) return false;
    if (fn.needsThis && !fn.isStrict) return false;
    // No new.target, no super(), no coroutine: what the direct edge requires.
    if (!il::functionEntersDirectly(fn)) return false;
    // A typed entry's source parameters arrive unboxed (typed_entry.h); the
    // site binds them to its own unboxed operands, which have those types.
    for (size_t p = 0; p < fn.params.size(); ++p) {
        const il::Type t = fn.params[p].type;
        const bool typedSource = fn.typedOrigin != il::Function::kNoTypedOrigin && p >= fn.firstSourceParam() &&
                                 (t == il::Type::F64 || t == il::Type::Bool);
        if (t != il::Type::Dynamic && !typedSource) return false;
    }
    if (fn.returnType != il::Type::Dynamic && fn.returnType != il::Type::Void) return false;
    if (instructionCount(fn) > kMaxCalleeInstructions) return false;

    const std::vector<il::Type> types = valueTypes(fn);
    std::vector<il::Op> defOp(fn.valueCount, il::Op::ConstUndefined);
    std::vector<uint8_t> defined(fn.valueCount, 0);
    for (const auto& b : fn.blocks) {
        for (const auto& inst : b.instructions) {
            if (inst.result != il::kNoValue && inst.result < fn.valueCount) {
                defOp[inst.result] = inst.op;
                defined[inst.result] = 1;
            }
        }
    }
    // A module-scope binding is reached from `module.env.get`, the one
    // record every function of the module sees the same way.
    const auto moduleEnv = [&](const il::Instruction& inst) {
        if (inst.operands.empty()) return false;
        const il::ValueId env = inst.operands[0];
        return env < fn.valueCount && defined[env] && defOp[env] == il::Op::ModuleEnvGet;
    };

    for (const auto& b : fn.blocks) {
        // A handler of its own would need its region re-parented inside the
        // caller's; the body leaves to the call's handler instead.
        if (b.handler != il::kNoBlock) return false;
        for (const auto& inst : b.instructions) {
            switch (inst.op) {
                case il::Op::EnvGet:
                case il::Op::EnvSet:
                case il::Op::EnvGetTdz:
                case il::Op::EnvInitTdz:
                    if (!moduleEnv(inst)) return false;
                    break;
                case il::Op::Call:
                    if (inst.callEnvHops != il::Instruction::kNoEnvHops) return false;
                    break;
                case il::Op::Ret:
                    if (!inst.operands.empty() && inst.operands[0] != il::kNoValue &&
                        (inst.operands[0] >= types.size() || types[inst.operands[0]] != il::Type::Dynamic)) {
                        return false;
                    }
                    break;
                // Each builds or reads something of the function's own
                // activation, its class, or its module's position.
                case il::Op::EnvCreate:
                case il::Op::CreateFunction:
                case il::Op::CoroStart:
                case il::Op::CoroSuspend:
                case il::Op::CoroMode:
                case il::Op::SuperGet:
                case il::Op::SuperSet:
                case il::Op::SuperCall:
                case il::Op::SuperCallSpread:
                case il::Op::GetNewTarget:
                case il::Op::ImportMeta:
                case il::Op::DynamicImport:
                case il::Op::ResolveName:
                case il::Op::ClassExtend:
                case il::Op::ModuleEnvSet:
                case il::Op::ExcTake:
                case il::Op::MethodIcIs:
                    return false;
                default:
                    break;
            }
        }
    }
    return true;
}

class Inliner {
public:
    Inliner(il::Module& module) : module_(module) {
        originals_.resize(module.functions.size());
        candidate_.assign(module.functions.size(), 0);
        for (size_t i = 0; i < module.functions.size(); ++i) {
            if (isCandidate(module.functions[i])) {
                candidate_[i] = 1;
                originals_[i] = module.functions[i];
            }
        }
    }

    // Inlines every eligible call in `fnIdx`, its copied bodies' calls
    // included, within the depth and size bounds. Returns how many.
    size_t inlineInto(uint32_t fnIdx) {
        il::Function& fn = module_.functions[fnIdx];
        if (fn.isExternal || fn.blocks.empty() || fn.isCoroutineBody()) return 0;
        // The top level is module initialisation, which runs once; a script
        // whose hot loop is there keeps its calls.
        if (fn.isEntryPoint || fn.name == "main") return 0;
        size_t count = instructionCount(fn);
        if (count > kMaxCallerStart) return 0;
        types_ = valueTypes(fn);
        slowBlock_.assign(fn.blocks.size(), 0);
        markLoops(fn);
        const size_t limit = std::min(kMaxCallerInstructions, count * 2 + kCallerGrowthSlack);
        size_t inlined = 0;
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            if (slowBlock_[b] || !inLoop_[b]) continue;
            for (size_t i = 0; i < fn.blocks[b].instructions.size(); ++i) {
                const il::Instruction& inst = fn.blocks[b].instructions[i];
                const uint32_t callee = eligibleCallee(fn, fnIdx, inst);
                if (callee == kNone) continue;
                const size_t grows = originals_[callee].blocks.size() * 2 + instructionCount(originals_[callee]) + 4;
                if (count + grows > limit) continue;
                inlineAt(fn, static_cast<il::BlockId>(b), i, callee);
                count += grows;
                ++inlined;
                // Block b now ends at the guard; the rest of it is the last
                // blocks appended, which the loop reaches in turn.
                break;
            }
        }
        return inlined;
    }

private:
    static constexpr uint32_t kNone = UINT32_MAX;

    uint32_t eligibleCallee(const il::Function& fn, uint32_t fnIdx, const il::Instruction& inst) const {
        // An accessor site (il.h `directTarget` on PropGet/PropSet) is a call
        // of its getter with no arguments, or of its setter with the value as
        // its one, behind its accessor cell instead of a method cache.
        const bool accessor = (inst.op == il::Op::PropGet || inst.op == il::Op::PropSet) &&
                              inst.directTarget != il::Instruction::kNoDirectTarget;
        if ((inst.op != il::Op::MethodCall && !accessor) || inst.operands.empty()) return kNone;
        const uint32_t origin = inst.directTarget;
        if (origin == il::Instruction::kNoDirectTarget || origin >= candidate_.size()) return kNone;
        // The body the call would run: the typed entry the site was pointed
        // at, else the generic function.
        const uint32_t callee = inst.typedTarget != il::Instruction::kNoDirectTarget ? inst.typedTarget : origin;
        if (callee >= candidate_.size() || !candidate_[callee]) return kNone;
        if (origin == originOf(fnIdx) || inst.operands.size() - 1 > kMaxArgs) return kNone;
        if (inst.result != il::kNoValue && inst.type != il::Type::Dynamic) return kNone;
        if (types_[inst.operands[0]] != il::Type::Dynamic) return kNone;
        const il::Function& body = originals_[callee];
        const size_t firstSource = body.firstSourceParam();
        for (size_t a = 1; a < inst.operands.size(); ++a) {
            const il::ValueId v = inst.operands[a];
            if (v >= types_.size()) return kNone;
            const size_t p = firstSource + (a - 1);
            // An argument past the declared parameters is dropped by the
            // inlined body; a real call would have evaluated it the same.
            const il::Type want = p < body.params.size() ? body.params[p].type : il::Type::Dynamic;
            if (types_[v] != want) return kNone;
        }
        // Not into itself, however deep: a recursive method keeps its call.
        size_t depth = 0;
        for (uint32_t f = inst.inlineFrame; f != 0; f = fn.inlineFrames[f - 1].parent) {
            if (fn.inlineFrames[f - 1].callee == origin) return kNone;
            ++depth;
        }
        if (depth >= kMaxDepth) return kNone;
        return callee;
    }

    // Which blocks lie in a loop: the calls a body copied in saves more than
    // once. A call outside every loop runs once per entry of its function, and
    // copying a body there costs build time for the price of one call.
    void markLoops(const il::Function& fn) {
        inLoop_.assign(fn.blocks.size(), 0);
        const Cfg cfg = buildCfg(fn);
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            if (cfg.rpoIndex[b] == UINT32_MAX) continue;
            for (il::BlockId h : cfg.succs[b]) {
                if (!cfg.dominates(h, static_cast<il::BlockId>(b))) continue;
                for (il::BlockId m : naturalLoop(cfg, h, static_cast<il::BlockId>(b))) inLoop_[m] = 1;
            }
        }
    }

    uint32_t originOf(uint32_t f) const {
        const uint32_t o = module_.functions[f].typedOrigin;
        return o == il::Function::kNoTypedOrigin ? f : o;
    }

    il::ValueId fresh(il::Function& fn, il::Type type) {
        const il::ValueId v = fn.valueCount++;
        types_.push_back(type);
        return v;
    }

    void inlineAt(il::Function& fn, il::BlockId b, size_t i, uint32_t calleeIdx) {
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
        const uint32_t origin = call.directTarget;
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
        const il::BlockId slow = newBlock();
        slowBlock_[slow] = 1;
        {
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

        // The callee's blocks, numbered after S, and every value it defines.
        const il::BlockId base = static_cast<il::BlockId>(fn.blocks.size());
        for (size_t k = 0; k < callee.blocks.size(); ++k) newBlock();
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
        if (callee.needsThis) vm[firstSource - 1] = call.operands[0];
        for (size_t p = firstSource; p < callee.params.size(); ++p) {
            const size_t arg = 1 + (p - firstSource);
            vm[p] = arg < call.operands.size() ? call.operands[arg] : undefinedAtHead();
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

        // B ends in the guard: the method cache's, or an accessor site's cell.
        {
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
                        il::ValueId v = (!src.operands.empty() && src.operands[0] != il::kNoValue)
                                            ? map(src.operands[0])
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
                if (inst.op == il::Op::Jump || inst.op == il::Op::Branch) {
                    inst.target = mapTarget(src.target);
                    if (inst.op == il::Op::Branch) inst.elseTarget = mapTarget(src.elseTarget);
                }
                out.instructions.push_back(std::move(inst));
            }
        }
    }

    il::Module& module_;
    std::vector<il::Function> originals_;
    // The blocks holding a guard's original call, whose call stays a call.
    std::vector<uint8_t> slowBlock_;
    std::vector<uint8_t> inLoop_;
    std::vector<uint8_t> candidate_;
    std::vector<il::Type> types_;
};

}  // namespace

bool inlineGuardedEnabled(const InlineGuardedOptions& options) {
    // BRONZE_NO_INLINE_GUARDED=1 builds the same image without it, the
    // A/B switch every inline path in this compiler has.
    return options.imageBuild && std::getenv("BRONZE_NO_INLINE_GUARDED") == nullptr;
}

bool inlineGuardedMethods(il::Module& module, DiagnosticSink& diags,
                          std::vector<uint32_t>* changed) {
    Inliner inliner(module);
    bool any = false;
    for (size_t f = 0; f < module.functions.size(); ++f) {
        if (inliner.inlineInto(static_cast<uint32_t>(f)) == 0) continue;
        any = true;
        if (changed) changed->push_back(static_cast<uint32_t>(f));
        if (!il::verifyFunction(module.functions[f], diags)) {
            if (std::getenv("BRONZE_INLINE_GUARDED_DEBUG")) {
                il::Module one;
                one.keyConstants = module.keyConstants;
                one.functions.push_back(module.functions[f]);
                std::fputs(il::print(one).c_str(), stderr);
            }
            diags.error(Span{}, "internal: guarded method inlining produced invalid IL in '" +
                                    module.functions[f].name + "'");
            return any;
        }
    }
    return any;
}

void foldInlinedCallers(il::Module& module, const std::vector<uint32_t>& changed) {
    for (uint32_t f : changed) {
        il::Function folded = module.functions[f];
        if (foldTypedEntry(folded) == 0 || !dominanceHolds(folded)) continue;
        module.functions[f] = std::move(folded);
    }
}

}  // namespace bronze::lower
