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
// A bimorphic site chains two guards over the same IC (`MethodIcIs` across the
// site's two direct ways, or `MethodSlotFn` followed by two `ClosureIs` tests),
// leaving S as the fallback call after the second miss.
//
// An ACCESSOR site (`prop.get` / `prop.set` naming its getter or setter, il.h
// `directTarget`) is inlined the same way, as a call of that body with the
// receiver as `this` and, for a setter, the stored value as its one argument.
// Its guard is `accessor.ic.is` on the site's cell pair, the test its own
// direct edge makes, and S keeps the original access, whose miss is what
// publishes the cell. A setter's return value is discarded, as [[Set]]
// discards it: a `prop.set` has no result for C to take.

#include "lower/inline_guarded_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

#include "abi/bronze_abi.h"
#include "il/print.h"
#include "il/verifier.h"

namespace bronze::lower {

namespace inline_guarded_detail {

namespace {

// The program's copy of a self-hosted builtin or of one of its helpers
// (selfhost_merge.cpp): the library's frames are BUILTIN or HIDDEN, which no
// function of the program's own is.
bool selfHosted(const il::Function& fn) {
    return (fn.descFlags & (BRONZE_FN_DESC_BUILTIN | BRONZE_FN_DESC_HIDDEN)) != 0;
}

}  // namespace

// Whether `fn`'s body means the same thing run at any method call that
// would have entered it: it reads nothing of the activation a real call
// builds beyond `this` and its parameters — and, for a CLOSURE body (one
// entered behind `closure.is`, "own callees" below), beyond the environment
// its function object closed over, which the inlined copy is handed.
// An EXACT body (a direct `call`'s, "exact calls" below) is bound to the
// call's operands one for one, whatever their types.
bool isCandidate(const il::Function& fn, bool closure, bool exact) {
    if (fn.isExternal || fn.blocks.empty() || fn.isCoroutineBody() || fn.isGenerator ||
        !fn.inlineFrames.empty()) {
        return false;
    }
    // The environment, `arguments` and the rest array are built by the call
    // wrapper from the real call; a sloppy function's `this` is coerced there.
    // A closure body's environment is its closure's (`closure.env`), and its
    // `this` is the receiver `method.slot.fn` matched to a SLOT entry's
    // shape, a plain object, which sloppy coercion leaves as it is.
    if ((fn.needsEnv && !closure) || fn.needsArguments || fn.hasRestParam) return false;
    if (fn.needsThis && !fn.isStrict && !closure) return false;
    // No new.target, no super(), no coroutine: what the direct edge requires.
    if (!il::functionEntersDirectly(fn)) return false;
    // A typed entry's source parameters arrive unboxed (typed_entry.h); the
    // site binds them to its own unboxed operands, which have those types.
    for (size_t p = 0; p < fn.params.size() && !exact; ++p) {
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
                    if (!closure && !moduleEnv(inst)) return false;
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
                case il::Op::AccessorIcIs:
                case il::Op::MethodSlotFn:
                case il::Op::ClosureIs:
                case il::Op::ClosureEnv:
                    return false;
                default:
                    break;
            }
        }
    }
    return true;
}

Inliner::Inliner(il::Module& module, const ClosureGuesses* guesses, const ColdBlocks* cold,
                 const ClosureGuesses* polyMethods, const ClosureGuesses* polyOwnCallees)
    : module_(module),
      guesses_(guesses),
      polyMethods_(polyMethods),
      polyOwnCallees_(polyOwnCallees),
      cold_(cold) {
    originals_.resize(module.functions.size());
    candidate_.assign(module.functions.size(), 0);
    closureCandidate_.assign(module.functions.size(), 0);
    std::vector<uint8_t> guessed(module.functions.size(), 0);
    const auto noteGuessed = [&](const ClosureGuesses* map) {
        if (!map) return;
        for (const auto& [site, fns] : *map) {
            (void)site;
            for (uint32_t f : fns) {
                if (f < guessed.size()) guessed[f] = 1;
            }
        }
    };
    noteGuessed(guesses_);
    noteGuessed(polyOwnCallees_);
    exactCandidate_.assign(module.functions.size(), 0);
    literalCandidate_.assign(module.functions.size(), 0);
    calleeHasLoop_.assign(module.functions.size(), 0);
    for (size_t i = 0; i < module.functions.size(); ++i) {
        const il::Function& f = module.functions[i];
        if (isCandidate(f, false)) candidate_[i] = 1;
        if (guessed[i] && isCandidate(f, true)) closureCandidate_[i] = 1;
        if (selfHosted(f) && isCandidate(f, false, /*exact=*/true)) exactCandidate_[i] = 1;
        // A callback's `this` is whatever the caller passed, which a
        // sloppy function would coerce; and a class constructor's [[Call]]
        // throws.
        if (isCandidate(f, true) && !(f.needsThis && !f.isStrict) &&
            (f.fnFlags & (BRONZE_ABI_FN_FLAG_CLASS_CTOR | BRONZE_ABI_FN_FLAG_GENERATOR | BRONZE_ABI_FN_FLAG_ASYNC)) ==
                0) {
            literalCandidate_[i] = 1;
        }
        if (candidate_[i] || closureCandidate_[i] || exactCandidate_[i] || literalCandidate_[i]) {
            originals_[i] = f;
            const auto lb = loopBlocks(f);
            if (std::any_of(lb.begin(), lb.end(), [](uint8_t v) { return v != 0; })) {
                calleeHasLoop_[i] = 1;
            }
        }
    }
    markEnteredFromLoops();
}

size_t Inliner::inlineInto(uint32_t fnIdx, std::unordered_set<uint32_t>* inlinedOwnIcs, bool tinyOnly) {
    il::Function& fn = module_.functions[fnIdx];
    if (fn.isExternal || fn.blocks.empty() || fn.isCoroutineBody()) return 0;
    // The top level is module initialisation, which runs once; a script
    // whose hot loop is there keeps its method calls, and takes only the
    // bodies that need no guard of their own (exact and literal calls).
    const bool entry = fn.isEntryPoint || fn.name == "main";
    markLoops(fn);
    const bool hot = enteredFromLoop_[originOf(fnIdx)] != 0;
    const bool hasLoop = std::any_of(inLoop_.begin(), inLoop_.end(), [](uint8_t v) { return v != 0; });
    const size_t initialCount = instructionCount(fn);
    size_t count = initialCount;
    const size_t maxCallerStart = (hasLoop || hot || tinyOnly) ? kMaxHotCallerStart : kMaxCallerStart;
    if (count > maxCallerStart) {
        if (trace()) std::fprintf(stderr, "[inline] %s: caller too big (%zu)\n", fn.name.c_str(), count);
        return 0;
    }
    // Callers already larger than `kMaxCallerStart` (such as outer benchmark
    // drivers or large batchers, or round-2 callers revisiting nested sites)
    // take only tiny loop-free callees so they never swallow a hot inner loop
    // function and starve it of its own callee budget and lean stack frame.
    const bool restrictToTiny = tinyOnly || initialCount > kMaxCallerStart;
    types_ = valueTypes(fn);
    literals_.clear();
    std::vector<il::Op> defOp(fn.valueCount, il::Op::ConstUndefined);
    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.instructions) {
            noteLiteral(inst);
            if (inst.result != il::kNoValue && inst.result < defOp.size()) defOp[inst.result] = inst.op;
        }
    }
    slowBlock_.assign(fn.blocks.size(), 0);
    // Preserve guard fallback blocks synthesized in an earlier round so their
    // fallback calls are never re-inlined during the second tiny-callee pass.
    const auto isGuardBranch = [&](const il::Block& blk) {
        if (blk.instructions.empty()) return false;
        const il::Instruction& term = blk.instructions.back();
        if (term.op != il::Op::Branch || term.operands.empty() || term.operands[0] >= defOp.size()) {
            return false;
        }
        const il::Op op = defOp[term.operands[0]];
        return op == il::Op::MethodIcIs || op == il::Op::AccessorIcIs || op == il::Op::ClosureIs;
    };
    for (const auto& blk : fn.blocks) {
        if (!isGuardBranch(blk)) continue;
        const il::BlockId miss = blk.instructions.back().elseTarget.block;
        if (miss < fn.blocks.size() && !isGuardBranch(fn.blocks[miss])) slowBlock_[miss] = 1;
    }
    closureTried_.assign(fn.blocks.size(), 0);
    methodTried_.assign(fn.blocks.size(), 0);
    closureSlotFn_.assign(fn.blocks.size(), il::kNoValue);
    reservedForBlock_.assign(fn.blocks.size(), 0);
    // Second-pass blocks: a guarded region's slow copy, and the generic
    // loop typed_loop_version.cpp copied per view kind, which runs only
    // once every copy's entry test has failed.
    lateBlock_.assign(fn.blocks.size(), 0);
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (fn.blocks[b].copyClass == il::CopyClass::Slow) lateBlock_[b] = 1;
    }
    if (cold_) {
        if (const auto it = cold_->find(fn.name); it != cold_->end()) {
            for (il::BlockId b : it->second) {
                if (b < lateBlock_.size()) lateBlock_[b] = 1;
            }
        }
    }
    const size_t baseLimit =
        std::min(kMaxCallerInstructions, std::max(count * 2 + kCallerGrowthSlack, count + kMinCallerGrowth));
    const size_t hotLimit =
        std::min(kMaxHotCallerInstructions, std::max(count * 4 + kCallerGrowthSlack, count + kMinHotCallerGrowth));
    size_t inlined = 0;
    // The guarded regions' fast copies first: the room is one budget for
    // the whole function, and a region's slow copy (its original blocks,
    // so the lower numbers) runs only once a guard of the fast copy has
    // failed.
    for (int pass = 0; pass < 2; ++pass) {
        size_t reserved = 0;
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            if ((b < lateBlock_.size() && lateBlock_[b]) != (pass == 1)) continue;
            if (b < reservedForBlock_.size() && reservedForBlock_[b] != 0) {
                reserved -= reservedForBlock_[b];
                reservedForBlock_[b] = 0;
            }
            if (slowBlock_[b]) continue;
            // Outside every loop of a function no loop enters, only an exact
            // call of a self-hosted builtin's copy is taken: its body is a
            // walk, and the callback it calls per element is the call worth
            // copying in.
            const bool onlyExact = !inLoop_[b] && !hot;
            // Open `+` chains: a call between a chain's links stays a call.
            // The chain's builder must be read by its next link in the same
            // block (il/verifier.cpp, verifyConcatChain), and a body copied
            // in would split the block between them.
            int openChains = 0;
            for (size_t i = 0; i < fn.blocks[b].instructions.size(); ++i) {
                const il::Instruction& inst = fn.blocks[b].instructions[i];
                if (inst.op == il::Op::ConcatBegin) ++openChains;
                if (inst.op == il::Op::ConcatEnd && openChains > 0) --openChains;
                if (openChains > 0) continue;
                Form unguardedForm = Form::Method;
                const uint32_t unguarded = unguardedCallee(fn, fnIdx, inst, unguardedForm);
                if (onlyExact && unguardedForm != Form::Exact) continue;
                const auto availableFor = [&](bool allowHotCap) -> size_t {
                    const size_t cap = (pass == 0 && allowHotCap) ? hotLimit : baseLimit;
                    return count + reserved <= cap ? cap - count - reserved : 0;
                };
                const auto roomFor = [&](uint32_t callee, size_t extra, bool exactCall) -> size_t {
                    const size_t calleeInsts = instructionCount(originals_[callee]);
                    if (!exactCall && calleeHasLoop_[callee] && calleeInsts > kTinyCalleeInstructions) {
                        return 0;
                    }
                    if (restrictToTiny && !exactCall &&
                        (calleeInsts > kTinyCalleeInstructions || calleeHasLoop_[callee])) {
                        return 0;
                    }
                    if (tinyOnly && exactCall && calleeInsts > kTinyCalleeInstructions) return 0;
                    const bool allowHot = inLoop_[b] || calleeInsts <= kTinyCalleeInstructions;
                    const size_t avail = availableFor(allowHot);
                    const size_t grows = growthOf(callee, extra);
                    if (grows <= avail) return grows;
                    if (trace()) {
                        const size_t cap = (pass == 0 && allowHot) ? hotLimit : baseLimit;
                        std::fprintf(stderr, "[inline] %s: no room for %s (%zu + %zu > %zu)\n", fn.name.c_str(),
                                     module_.functions[callee].name.c_str(), count + reserved, grows, cap);
                    }
                    return 0;
                };
                const auto polyFitsTiny = [&](const ClosureGuesses* map) {
                    if (!restrictToTiny) return true;
                    if (!map) return false;
                    const auto it = map->find(inst.icIndex);
                    if (it == map->end()) return false;
                    for (uint32_t t : it->second) {
                        if (t >= originals_.size() || calleeHasLoop_[t] ||
                            instructionCount(originals_[t]) > kTinyCalleeInstructions) {
                            return false;
                        }
                    }
                    return true;
                };
                SplicePlan splice{};
                size_t grows = 0;
                uint32_t callee = kNone;
                if (unguarded != kNone) {
                    if ((grows = roomFor(unguarded, 0, unguardedForm == Form::Exact)) == 0) continue;
                    callee = unguarded;
                    splice.form = unguardedForm;
                } else if (entry) {
                    continue;
                } else {
                    const il::BlockId bid = static_cast<il::BlockId>(b);
                    const bool polyOwn = polyOwnCallees_ != nullptr && polyOwnCallees_->count(inst.icIndex) != 0;
                    if (polyOwn && polyFitsTiny(polyOwnCallees_)) {
                        const size_t polyAvail = availableFor(inLoop_[b] || restrictToTiny);
                        callee = polyClosureCallee(fn, fnIdx, bid, i, inst, polyAvail, splice, grows);
                    }
                    if (callee == kNone && !polyOwn) {
                        uint32_t next = 0;
                        const uint32_t c = closureCallee(fn, fnIdx, bid, inst, next);
                        if (c != kNone) {
                            const bool reuse =
                                i == 0 && b < closureSlotFn_.size() && closureSlotFn_[b] != il::kNoValue;
                            grows = roomFor(c, reuse ? kChainedClosureGuardInstructions : kClosureGuardInstructions,
                                            /*exactCall=*/false);
                            if (grows != 0) {
                                callee = c;
                                splice = SplicePlan{Form::Closure, next, /*moreTries=*/true, 0};
                            }
                        }
                    }
                    if (callee == kNone) {
                        const uint32_t m = eligibleCallee(fn, fnIdx, inst);
                        if (m != kNone && (grows = roomFor(m, 0, /*exactCall=*/false)) != 0) {
                            callee = m;
                            splice = SplicePlan{Form::Method, 0, /*moreTries=*/false, 0};
                        }
                    }
                    if (callee == kNone && polyFitsTiny(polyMethods_)) {
                        const size_t polyAvail = availableFor(inLoop_[b] || restrictToTiny);
                        callee = polyMethodCallee(fn, fnIdx, bid, inst, polyAvail, splice, grows);
                    }
                    if (callee == kNone) continue;
                }
                if (trace()) {
                    static const char* const kForm[] = {"", "closure ", "exact ", "literal "};
                    std::fprintf(stderr, "[inline] %s <- %s%s (%zu)%s%s\n", fn.name.c_str(),
                                 kForm[static_cast<int>(splice.form)], module_.functions[callee].name.c_str(),
                                 instructionCount(originals_[callee]), inLoop_[b] ? "" : " outside loops",
                                 pass == 1 ? " late" : "");
                }
                const uint32_t callIc = inst.icIndex;
                const uint8_t late = b < lateBlock_.size() ? lateBlock_[b] : 0;
                inlineAt(fn, static_cast<il::BlockId>(b), i, callee, splice);
                if (splice.form == Form::Closure && inlinedOwnIcs != nullptr) inlinedOwnIcs->insert(callIc);
                lateBlock_.resize(fn.blocks.size(), late);
                count += grows;
                reserved += splice.reserveSlow;
                ++inlined;
                break;
            }
        }
    }
    return inlined;
}

// EXACT CALLS and LITERAL CALLBACKS: bodies copied in with no guard,
// because the call names its callee outright.
uint32_t Inliner::unguardedCallee(const il::Function& fn, uint32_t fnIdx, const il::Instruction& inst, Form& form) {
    if (inst.result != il::kNoValue && inst.type != il::Type::Dynamic) return kNone;
    uint32_t target = kNone;
    if (inst.op == il::Op::Call && inst.callEnvHops == il::Instruction::kNoEnvHops &&
        inst.calleeIndex < exactCandidate_.size() && exactCandidate_[inst.calleeIndex]) {
        target = inst.calleeIndex;
        form = Form::Exact;
    } else if (inst.op == il::Op::DynamicCall && inst.operands.size() >= 2 && inst.operands.size() - 2 <= kMaxArgs) {
        const auto def = literals_.find(inst.operands[0]);
        if (def == literals_.end() || def->second.first >= literalCandidate_.size() ||
            !literalCandidate_[def->second.first]) {
            return kNone;
        }
        target = def->second.first;
        const il::Function& body = originals_[target];
        if (body.needsEnv && def->second.second == il::kNoValue) return kNone;
        for (size_t a = 1; a < inst.operands.size(); ++a) {
            if (inst.operands[a] >= types_.size() || types_[inst.operands[a]] != il::Type::Dynamic) return kNone;
        }
        form = Form::Literal;
    } else {
        return kNone;
    }
    const uint32_t origin = originOf(target);
    if (origin == originOf(fnIdx)) return kNone;
    size_t depth = 0;
    for (uint32_t f = inst.inlineFrame; f != 0; f = fn.inlineFrames[f - 1].parent) {
        if (fn.inlineFrames[f - 1].callee == origin) return kNone;
        ++depth;
    }
    return depth >= kMaxDepth ? kNone : target;
}

bool Inliner::eligibleMethodTarget(const il::Function& fn, uint32_t fnIdx, const il::Instruction& inst,
                                   uint32_t origin, uint32_t callee) const {
    if (origin >= candidate_.size() || callee >= candidate_.size() || !candidate_[callee]) return false;
    if (calleeHasLoop_[callee] && instructionCount(originals_[callee]) > kTinyCalleeInstructions) return false;
    if (origin == originOf(fnIdx) || inst.operands.empty() || inst.operands.size() - 1 > kMaxArgs) return false;
    if (inst.result != il::kNoValue && inst.type != il::Type::Dynamic) return false;
    if (inst.operands[0] >= types_.size() || types_[inst.operands[0]] != il::Type::Dynamic) return false;
    const il::Function& body = originals_[callee];
    const size_t firstSource = body.firstSourceParam();
    for (size_t a = 1; a < inst.operands.size(); ++a) {
        const il::ValueId v = inst.operands[a];
        if (v >= types_.size()) return false;
        const size_t p = firstSource + (a - 1);
        const il::Type want = p < body.params.size() ? body.params[p].type : il::Type::Dynamic;
        if (types_[v] != want) return false;
    }
    size_t depth = 0;
    for (uint32_t f = inst.inlineFrame; f != 0; f = fn.inlineFrames[f - 1].parent) {
        if (fn.inlineFrames[f - 1].callee == origin) return false;
        ++depth;
    }
    return depth < kMaxDepth;
}

uint32_t Inliner::eligibleCallee(const il::Function& fn, uint32_t fnIdx, const il::Instruction& inst) const {
    const bool accessor = (inst.op == il::Op::PropGet || inst.op == il::Op::PropSet) &&
                          inst.directTarget != il::Instruction::kNoDirectTarget;
    if ((inst.op != il::Op::MethodCall && !accessor) || inst.operands.empty()) return kNone;
    const uint32_t origin = inst.directTarget;
    if (origin == il::Instruction::kNoDirectTarget) return kNone;
    const uint32_t callee = inst.typedTarget != il::Instruction::kNoDirectTarget ? inst.typedTarget : origin;
    return eligibleMethodTarget(fn, fnIdx, inst, origin, callee) ? callee : kNone;
}

uint32_t Inliner::polyMethodCallee(const il::Function& fn, uint32_t fnIdx, il::BlockId b, const il::Instruction& inst,
                                   size_t available, SplicePlan& splice, size_t& grows) const {
    if (!polyMethods_ || inst.op != il::Op::MethodCall || inst.directTarget != il::Instruction::kNoDirectTarget) {
        return kNone;
    }
    const auto it = polyMethods_->find(inst.icIndex);
    if (it == polyMethods_->end() || it->second.size() < 2) return kNone;
    const auto& targets = it->second;
    const uint32_t k = b < methodTried_.size() ? methodTried_[b] : 0;
    if (k >= targets.size()) return kNone;
    // At the first arm, require every arm of the bimorphic site to pass
    // eligibility and fit together so a 50/50 split never leaves half its
    // calls behind a guard miss.
    const size_t start = k == 0 ? 0 : k;
    size_t total = 0;
    for (size_t j = start; j < targets.size(); ++j) {
        const uint32_t t = targets[j];
        if (!eligibleMethodTarget(fn, fnIdx, inst, originOf(t), t)) return kNone;
        total += growthOf(t, 0);
    }
    if (total > available) {
        if (trace()) {
            std::fprintf(stderr, "[inline] %s: no room for bimorphic method site (%zu > %zu)\n", fn.name.c_str(),
                         total, available);
        }
        return kNone;
    }
    const uint32_t chosen = targets[k];
    grows = growthOf(chosen, 0);
    splice = SplicePlan{Form::Method, k + 1, k + 1 < targets.size(), total - grows};
    return chosen;
}

bool Inliner::eligibleClosureTarget(const il::Function& fn, uint32_t fnIdx, const il::Instruction& inst,
                                    uint32_t target) const {
    if (inst.op != il::Op::MethodCall || inst.operands.empty() || inst.operands.size() - 1 > kMaxArgs) return false;
    if (inst.result != il::kNoValue && inst.type != il::Type::Dynamic) return false;
    if (inst.operands[0] >= types_.size() || types_[inst.operands[0]] != il::Type::Dynamic) return false;
    for (size_t a = 1; a < inst.operands.size(); ++a) {
        if (inst.operands[a] >= types_.size() || types_[inst.operands[a]] != il::Type::Dynamic) return false;
    }
    if (target >= closureCandidate_.size() || !closureCandidate_[target] || target == originOf(fnIdx)) return false;
    if (calleeHasLoop_[target] && instructionCount(originals_[target]) > kTinyCalleeInstructions) return false;
    size_t depth = 0;
    for (uint32_t f = inst.inlineFrame; f != 0; f = fn.inlineFrames[f - 1].parent) {
        if (fn.inlineFrames[f - 1].callee == target) return false;
        ++depth;
    }
    return depth < kMaxDepth;
}

uint32_t Inliner::polyClosureCallee(const il::Function& fn, uint32_t fnIdx, il::BlockId b, size_t instIdx,
                                    const il::Instruction& inst, size_t available, SplicePlan& splice,
                                    size_t& grows) const {
    if (!polyOwnCallees_ || inst.op != il::Op::MethodCall) return kNone;
    const auto it = polyOwnCallees_->find(inst.icIndex);
    if (it == polyOwnCallees_->end() || it->second.size() < 2) return kNone;
    const auto& targets = it->second;
    const uint32_t k = b < closureTried_.size() ? closureTried_[b] : 0;
    if (k >= targets.size()) return kNone;
    const bool reuseHere = instIdx == 0 && b < closureSlotFn_.size() && closureSlotFn_[b] != il::kNoValue;
    size_t total = 0;
    for (size_t j = k; j < targets.size(); ++j) {
        const uint32_t t = targets[j];
        if (!eligibleClosureTarget(fn, fnIdx, inst, t)) return kNone;
        const size_t extra =
            (j == k ? reuseHere : true) ? kChainedClosureGuardInstructions : kClosureGuardInstructions;
        total += growthOf(t, extra);
    }
    if (total > available) {
        if (trace()) {
            std::fprintf(stderr, "[inline] %s: no room for bimorphic closure site (%zu > %zu)\n", fn.name.c_str(),
                         total, available);
        }
        return kNone;
    }
    const uint32_t chosen = targets[k];
    grows = growthOf(chosen, reuseHere ? kChainedClosureGuardInstructions : kClosureGuardInstructions);
    splice = SplicePlan{Form::Closure, k + 1, k + 1 < targets.size(), total - grows};
    return chosen;
}

// OWN CALLEES. A `method.call` whose key the program stores closures under
// (`closureGuesses` or `ownCallees`) may find its callee in the receiver's own
// slot rather than on its prototype: three.js's `this._onChangeCallback()`,
// `PropertyMixer._mixBufferRegion`, `PropertyBinding.setValue`. Each receiver
// may hold a distinct closure object of the same function T, tested via
// `method.slot.fn` + `closure.is` and entered with `closure.env`.
uint32_t Inliner::closureCallee(const il::Function& fn, uint32_t fnIdx, il::BlockId b, const il::Instruction& inst,
                                uint32_t& next) const {
    if (!guesses_ || inst.op != il::Op::MethodCall || inst.operands.empty()) return kNone;
    const auto it = guesses_->find(inst.icIndex);
    if (it == guesses_->end()) return kNone;
    for (size_t k = closureTried_[b]; k < it->second.size(); ++k) {
        const uint32_t t = it->second[k];
        if (!eligibleClosureTarget(fn, fnIdx, inst, t)) continue;
        next = static_cast<uint32_t>(k + 1);
        return t;
    }
    return kNone;
}

// Which functions a loop enters by a real call, so that the calls outside
// their own loops run once per iteration of that loop and are as hot as
// its calls are.
void Inliner::markEnteredFromLoops() {
    enteredFromLoop_.assign(module_.functions.size(), 0);
    const auto mark = [&](uint32_t f, const il::Function& from) {
        if (f >= enteredFromLoop_.size()) return;
        if (!enteredFromLoop_[f] && trace()) {
            std::fprintf(stderr, "[inline] hot: %s (from a loop in %s)\n", module_.functions[f].name.c_str(),
                         from.name.c_str());
        }
        enteredFromLoop_[f] = 1;
    };
    for (size_t g = 0; g < module_.functions.size(); ++g) {
        const il::Function& fn = module_.functions[g];
        if (fn.isExternal || fn.blocks.empty() || fn.isCoroutineBody()) continue;
        markLoops(fn);
        const size_t callerInsts = instructionCount(fn);
        const bool callerSkipsAllGuarded =
            fn.isEntryPoint || fn.name == "main" || callerInsts > kMaxHotCallerStart;
        const bool callerOnlyTiny = callerInsts > kMaxCallerStart;
        const auto callerSkipsCallee = [&](uint32_t callee) {
            if (callerSkipsAllGuarded) return true;
            if (callee < originals_.size() && calleeHasLoop_[callee] &&
                instructionCount(originals_[callee]) > kTinyCalleeInstructions) {
                return true;
            }
            if (!callerOnlyTiny) return false;
            return callee >= originals_.size() || calleeHasLoop_[callee] ||
                   instructionCount(originals_[callee]) > kTinyCalleeInstructions;
        };
        const uint32_t callerOrigin = originOf(static_cast<uint32_t>(g));
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            if (!inLoop_[b]) continue;
            for (const auto& inst : fn.blocks[b].instructions) {
                const uint32_t origin = inst.directTarget;
                if (origin == il::Instruction::kNoDirectTarget) {
                    // A plain call of a module function (`updateTransformAndChildren(child, ...)`
                    // per child, and from itself): entered once per iteration
                    // unless its body is copied in here.
                    if (inst.op == il::Op::Call && inst.callEnvHops == il::Instruction::kNoEnvHops &&
                        inst.calleeIndex < exactCandidate_.size()) {
                        const uint32_t to = originOf(inst.calleeIndex);
                        if (callerSkipsCallee(inst.calleeIndex) || to == callerOrigin ||
                            !exactCandidate_[inst.calleeIndex]) {
                            mark(to, fn);
                        }
                        continue;
                    }
                    // A call of a known closure (`projectObject(children[i], ...)`
                    // behind a `closure.is` of the slot that holds it): its body
                    // is never copied in, so it is entered once per iteration.
                    if (inst.op == il::Op::Call && inst.calleeIndex < module_.functions.size()) {
                        mark(originOf(inst.calleeIndex), fn);
                        continue;
                    }
                    if (inst.op != il::Op::MethodCall) continue;
                    if (fn.needsThis && inst.keyIndex == fn.nameKeyIndex) mark(callerOrigin, fn);
                    const auto markUninlined = [&](const ClosureGuesses* map, const std::vector<uint8_t>& ok) {
                        if (!map) return;
                        const auto it = map->find(inst.icIndex);
                        if (it == map->end()) return;
                        for (uint32_t t : it->second) {
                            const uint32_t to = originOf(t);
                            if (callerSkipsCallee(t) || to == callerOrigin || t >= ok.size() || !ok[t]) mark(to, fn);
                        }
                    };
                    markUninlined(polyMethods_, candidate_);
                    markUninlined(polyOwnCallees_, closureCandidate_);
                    markUninlined(guesses_, closureCandidate_);
                    continue;
                }
                const bool site =
                    inst.op == il::Op::MethodCall || inst.op == il::Op::PropGet || inst.op == il::Op::PropSet;
                if (!site || origin >= candidate_.size()) continue;
                const uint32_t callee = inst.typedTarget != il::Instruction::kNoDirectTarget ? inst.typedTarget : origin;
                if (callerSkipsCallee(callee) || origin == callerOrigin || callee >= candidate_.size() ||
                    !candidate_[callee]) {
                    mark(origin, fn);
                }
            }
        }
    }
}

}  // namespace inline_guarded_detail

bool inlineGuardedEnabled(const InlineGuardedOptions& options) { return options.on; }

bool inlineGuardedMethods(il::Module& module, DiagnosticSink& diags, std::vector<uint32_t>* changed,
                          const ClosureGuesses* guesses, const ColdBlocks* cold, const ClosureGuesses* polyMethods,
                          const ClosureGuesses* polyOwnCallees, std::unordered_set<uint32_t>* inlinedOwnIcs) {
    inline_guarded_detail::Inliner inliner(module, guesses, cold, polyMethods, polyOwnCallees);
    bool any = false;
    const char* dump = std::getenv("BRONZE_INLINE_DUMP");
    std::vector<uint32_t> round1Changed;
    const auto runOne = [&](uint32_t f, bool tinyOnly, std::vector<uint32_t>* outChanged) {
        il::Function before = module.functions[f];
        std::unordered_set<uint32_t> localOwnIcs;
        if (inliner.inlineInto(f, inlinedOwnIcs ? &localOwnIcs : nullptr, tinyOnly) == 0) return;
        // A guessed callee can be one this site never calls (a profile that
        // is stale or wrong names it), and its body copied over this site's
        // arguments can hold an operation whose certificate they contradict:
        // an element store on what is here a boxed number. The guard would
        // never pass, but the IL has to stand on its own, so the caller keeps
        // its calls. A wrong guess costs speed, never a build.
        if (DiagnosticSink refused; !il::verifyFunction(module.functions[f], refused)) {
            if (std::getenv("BRONZE_INLINE_GUARDED_DEBUG")) {
                il::Module one;
                one.keyConstants = module.keyConstants;
                one.functions.push_back(module.functions[f]);
                std::fputs(il::print(one).c_str(), stderr);
            }
            module.functions[f] = std::move(before);
            return;
        }
        if (inlinedOwnIcs) inlinedOwnIcs->insert(localOwnIcs.begin(), localOwnIcs.end());
        any = true;
        if (outChanged) outChanged->push_back(f);
    };

    for (size_t f = 0; f < module.functions.size(); ++f) {
        runOne(static_cast<uint32_t>(f), /*tinyOnly=*/false, &round1Changed);
    }
    if (!round1Changed.empty()) {
        // Fold callers modified in round 1 and run a targeted tiny-callee pass
        // over them so nested accessors and leaf helpers (`Container.x` ->
        // `ObservablePoint.x` -> `_onUpdate`) flatten cleanly.
        foldInlinedCallers(module, round1Changed);
        std::vector<uint32_t> round2Changed;
        for (uint32_t f : round1Changed) runOne(f, /*tinyOnly=*/true, &round2Changed);
        if (!round2Changed.empty()) foldInlinedCallers(module, round2Changed);
    }
    if (dump) {
        for (uint32_t f : round1Changed) {
            if (module.functions[f].name != dump) continue;
            il::Module one;
            one.keyConstants = module.keyConstants;
            one.functions.push_back(module.functions[f]);
            std::fputs(il::print(one).c_str(), stderr);
        }
    }
    if (changed) changed->insert(changed->end(), round1Changed.begin(), round1Changed.end());
    (void)diags;
    return any;
}

}  // namespace bronze::lower
