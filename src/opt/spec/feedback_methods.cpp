// The method-call half of type-feedback specialization (feedback_plan.h): the
// direct targets (B), applied before the passes that copy bodies, and the
// profiled `call.dynamic` callee a closure split takes.

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "opt/spec/feedback_plan.h"
#include "opt/spec/feedback_util.h"

namespace bronze::spec {

namespace {

constexpr uint32_t kNone = UINT32_MAX;

// The profile's one callee for `names`, as a module function, or kNone.
uint32_t calleeOf(const FeedbackPlan& plan, const NameCounts& names) {
    const std::string* name = names.dominant(kDominantPercent);
    if (name == nullptr) return kNone;
    const auto it = plan.callees.find(*name);
    return it == plan.callees.end() ? kNone : it->second;
}

// Two distinct compiled callees that together account for at least
// `kDominantPercent` of `names`, ordered more frequent first.
bool bimorphicCallees(const FeedbackPlan& plan, const NameCounts& names, uint32_t (&out)[2]) {
    if (names.total < kMinExecutions) return false;
    std::pair<uint64_t, const std::string*> top[2] = {{0, nullptr}, {0, nullptr}};
    for (const auto& [name, count] : names.counts) {
        if (count == 0 || name == "*other*") continue;
        if (count > top[0].first) {
            top[1] = top[0];
            top[0] = {count, &name};
        } else if (count > top[1].first) {
            top[1] = {count, &name};
        }
    }
    if (top[0].second == nullptr || top[1].second == nullptr) return false;
    if ((top[0].first + top[1].first) * 100 < names.total * kDominantPercent) return false;
    const auto c0 = plan.callees.find(*top[0].second);
    const auto c1 = plan.callees.find(*top[1].second);
    if (c0 == plan.callees.end() || c1 == plan.callees.end() || c0->second == c1->second) return false;
    out[0] = c0->second;
    out[1] = c1->second;
    return true;
}

// Where a method site's receivers held its key, from the shapes the run saw
// there (each at least 1% of the executions): Own when some shape's complete
// key list has it, Inherited when every one's is complete and lacks it, else
// Unknown (a list the recorder cut short, a receiver named by its kind).
enum class KeyHome { Unknown, Own, Inherited };

KeyHome methodKeyHome(const FeedbackPlan& plan, uint32_t ic, const std::string& key) {
    const auto it = plan.methodShapesByIc.find(ic);
    if (it == plan.methodShapesByIc.end() || it->second.total == 0) return KeyHome::Unknown;
    bool all_lack = true;
    std::vector<std::string_view> keys;
    for (const auto& [name, count] : it->second.counts) {
        if (count * 100 < it->second.total) continue;
        bool complete = false;
        if (name == "*other*" || !shapeKeys(name, keys, complete)) {
            all_lack = false;
            continue;
        }
        bool has = false;
        for (std::string_view k : keys) has = has || k == key;
        if (has && complete) return KeyHome::Own;
        if (has || !complete) all_lack = false;
    }
    return all_lack ? KeyHome::Inherited : KeyHome::Unknown;
}

// For a bimorphic site, both guards must use the same dispatch form: either
// every major receiver shape is plain and holds the key as its own (AllOwn ->
// slot closure guards), or every major receiver shape is plain and lacks it
// (AllInherited / InheritedOrTruncated -> method-cache direct guards). A site
// mixing own and inherited receivers, or non-plain receivers, takes neither.
enum class PolyShapeHome { NoShapes, AllInherited, InheritedOrTruncated, AllOwn, Mixed };

PolyShapeHome polyMethodShapeHome(const FeedbackPlan& plan, uint32_t ic, const std::string& key) {
    const auto it = plan.methodShapesByIc.find(ic);
    if (it == plan.methodShapesByIc.end() || it->second.total == 0) return PolyShapeHome::NoShapes;
    bool anyMajor = false;
    bool anyOwn = false;
    bool allOwn = true;
    bool allCompleteLack = true;
    std::vector<std::string_view> keys;
    for (const auto& [name, count] : it->second.counts) {
        if (count * 100 < it->second.total) continue;
        anyMajor = true;
        bool complete = false;
        if (name == "*other*" || !shapeKeys(name, keys, complete)) return PolyShapeHome::Mixed;
        bool has = false;
        for (std::string_view k : keys) has = has || k == key;
        if (has) {
            anyOwn = true;
            allCompleteLack = false;
            if (!complete) allOwn = false;
        } else {
            allOwn = false;
            if (!complete) allCompleteLack = false;
        }
    }
    if (!anyMajor) return PolyShapeHome::NoShapes;
    if (allOwn) return PolyShapeHome::AllOwn;
    if (anyOwn) return PolyShapeHome::Mixed;
    return allCompleteLack ? PolyShapeHome::AllInherited : PolyShapeHome::InheritedOrTruncated;
}

// Whether the run saw a method site meet two or more plain receiver shapes,
// each at least 1% of its executions.
bool polyReceivers(const FeedbackPlan& plan, uint32_t ic) {
    const auto it = plan.methodShapesByIc.find(ic);
    if (it == plan.methodShapesByIc.end() || it->second.total < kMinExecutions) return false;
    uint32_t n = 0;
    std::vector<std::string_view> keys;
    for (const auto& [name, count] : it->second.counts) {
        bool complete = false;
        if (count * 100 >= it->second.total && name != "*other*" && shapeKeys(name, keys, complete)) ++n;
    }
    return n >= 2;
}

bool fitsDirectMethod(const il::Module& mod, const il::Instruction& inst, uint32_t target) {
    if (target >= mod.functions.size()) return false;
    const il::Function& callee = mod.functions[target];
    const size_t argc = inst.operands.size() - 1;
    bool fits = !callee.needsEnv && !callee.needsArguments && !callee.hasRestParam && !callee.isExternal;
    const size_t base = callee.firstSourceParam();
    for (size_t i = argc; fits && i < callee.callerParamCount(); ++i) {
        if (callee.params[i + base].type != il::Type::Dynamic) fits = false;
    }
    return fits;
}

}  // namespace

void applyFeedbackDirectMethods(il::Module& mod, const FeedbackPlan& plan, std::unordered_map<uint32_t, uint32_t>& ownCallees,
                                FeedbackApplyStats* stats,
                                std::unordered_map<uint32_t, std::vector<uint32_t>>* polyMethods,
                                std::unordered_map<uint32_t, std::vector<uint32_t>>* polyOwnCallees) {
    std::unordered_set<uint32_t> planned;
    for (il::Function& fn : mod.functions) {
        for (il::Block& block : fn.blocks) {
            for (il::Instruction& inst : block.instructions) {
                if (inst.op != il::Op::MethodCall || inst.operands.empty()) continue;
                if (polyReceivers(plan, inst.icIndex)) inst.immI32 |= il::Instruction::kMethodPolyShapes;
                const auto it = plan.methodsByIc.find(inst.icIndex);
                if (it == plan.methodsByIc.end()) continue;
                if (inst.directTarget != il::Instruction::kNoDirectTarget) {
                    // A predicted target the run never entered: the callee it
                    // saw is native (a compiled one is named `name@hash`), so
                    // the target's guard misses every time and its miss is
                    // the generic call. Without it the site gets its ordinary
                    // dispatch, the builtin's inline arm included.
                    const std::string* seen = it->second.dominant(kDominantPercent);
                    if (seen != nullptr && seen->find('@') == std::string::npos &&
                        inst.typedTarget == il::Instruction::kNoDirectTarget) {
                        inst.directTarget = il::Instruction::kNoDirectTarget;
                        if (stats) ++stats->nativeTargetsDropped;
                    }
                    continue;
                }
                const KeyHome home = inst.keyIndex < mod.keyConstants.size()
                                         ? methodKeyHome(plan, inst.icIndex, mod.keyConstants[inst.keyIndex])
                                         : KeyHome::Unknown;
                const bool predicted = ownCallees.count(inst.icIndex) != 0;
                const bool own_hint = (inst.immI32 & il::Instruction::kMethodOwnCallee) != 0;
                const uint32_t target = calleeOf(plan, it->second);
                if (target == kNone) {
                    // Receivers that hold the key in an own slot, with no one
                    // closure in it: the SLOT form is still dispatched at the
                    // site, entering whichever closure the slot holds.
                    if (home == KeyHome::Own && !own_hint) {
                        inst.immI32 |= il::Instruction::kMethodOwnCallee;
                        if (stats) ++stats->ownSlotSites;
                    }
                    uint32_t pair[2];
                    if (bimorphicCallees(plan, it->second, pair) && inst.keyIndex < mod.keyConstants.size()) {
                        const PolyShapeHome polyHome =
                            polyMethodShapeHome(plan, inst.icIndex, mod.keyConstants[inst.keyIndex]);
                        if (polyHome == PolyShapeHome::AllOwn) {
                            ownCallees.erase(inst.icIndex);
                            if (polyOwnCallees) (*polyOwnCallees)[inst.icIndex] = {pair[0], pair[1]};
                        } else if (polyHome == PolyShapeHome::AllInherited ||
                                   ((polyHome == PolyShapeHome::InheritedOrTruncated ||
                                     polyHome == PolyShapeHome::NoShapes) &&
                                    !predicted && !own_hint)) {
                            if (fitsDirectMethod(mod, inst, pair[0]) && fitsDirectMethod(mod, inst, pair[1])) {
                                ownCallees.erase(inst.icIndex);
                                inst.immI32 &= ~il::Instruction::kMethodOwnCallee;
                                if (polyMethods) (*polyMethods)[inst.icIndex] = {pair[0], pair[1]};
                            }
                        }
                    }
                    continue;
                }
                // The run's receivers held the callee in an own slot (a
                // factory's returned literal, WebGLProperties' `get`): the
                // method cache latches the SLOT form there, which no direct
                // target's guard (a DIRECT latch) ever matches. Split on the
                // slot's closure instead (own_callee_calls.h), as the
                // whole-program prediction does for the sites it names.
                if (home == KeyHome::Own) {
                    if (!predicted) {
                        ownCallees[inst.icIndex] = target;
                        if (stats) ++stats->ownCallees;
                    }
                    continue;
                }
                // A site the prediction gave an own-slot callee, or marked as
                // one whose key some object writes as its own, keeps that
                // unless every receiver the run saw inherited the key.
                if ((predicted || own_hint) && home != KeyHome::Inherited) continue;
                // The terms of a whole-program direct target (wpo/stamp.cpp,
                // planDirect): an entry a fixed operand list can fill. The
                // guard is the site's method cache, whose direct entry names
                // this code only for a receiver whose lookup finds it.
                // Arguments past the parameters are dropped by the direct
                // edge: with no `arguments` object and no rest parameter the
                // callee cannot observe them, and they are evaluated already.
                if (!fitsDirectMethod(mod, inst, target)) {
                    if (stats) ++stats->methodRefused;
                    continue;
                }
                inst.directTarget = target;
                inst.immI32 &= ~il::Instruction::kMethodOwnCallee;
                ownCallees.erase(inst.icIndex);
                if (stats) {
                    ++stats->directMethods;
                    if (planned.insert(inst.icIndex).second) stats->methodExecutions += it->second.total;
                }
            }
        }
    }
}

uint32_t profiledClosureTarget(const il::Module& mod, const FeedbackPlan& plan, const il::Instruction& call) {
    if (call.op != il::Op::DynamicCall || call.operands.size() < 2) return kNone;
    const auto it = plan.dynamicCallees.find(spanKey(call.span));
    if (it == plan.dynamicCallees.end()) return kNone;
    const uint32_t target = calleeOf(plan, it->second);
    if (target == kNone || target >= mod.functions.size()) return kNone;
    // A sloppy function's `this` is the global object when the call passes
    // undefined, which its generic entry supplies and the direct one does not.
    const il::Function& callee = mod.functions[target];
    if (callee.needsThis && !callee.isStrict) return kNone;
    return target;
}

}  // namespace bronze::spec
