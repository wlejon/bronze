// The consuming half of type-feedback specialization (feedback_plan.h): the
// direct targets (B) before the passes that copy bodies, and the static-slot
// claims (A) and number hints (C) on the finished module.

#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "il/feedback_site.h"
#include "il/key.h"
#include "opt/spec/feedback_plan.h"
#include "opt/spec/feedback_util.h"
#include "opt/wpo/stamp.h"

namespace bronze::spec {

namespace {

constexpr uint32_t kNone = UINT32_MAX;

// A shape name `<prototype>{k1,k2,...}` (runtime/type_feedback.cpp) split into
// its keys, in slot order. False for a dictionary, a receiver named by kind
// (`array`, `number`, ...) or an unparsable name. `complete` is false when the
// recorder cut the list short (`+N`).
bool shapeKeys(const std::string& name, std::vector<std::string_view>& keys, bool& complete) {
    const size_t open = name.find('{');
    if (open == std::string::npos || name.empty() || name.back() != '}') return false;
    const std::string_view body = std::string_view(name).substr(open + 1, name.size() - open - 2);
    if (body == "dict" || body == "?") return false;
    keys.clear();
    complete = true;
    if (body.empty()) return true;
    size_t at = 0;
    while (at <= body.size()) {
        const size_t comma = std::min(body.find(',', at), body.size());
        const std::string_view key = body.substr(at, comma - at);
        if (comma == body.size() && !key.empty() && key.front() == '+') {
            complete = false;
            break;
        }
        keys.push_back(key);
        at = comma + 1;
    }
    return true;
}

// The instructions strictly inside a transition run: nothing may be claimed
// there, the run's own guard answers them (lower/transition_runs.h).
std::vector<uint8_t> runInteriors(const il::Block& block) {
    std::vector<uint8_t> interior(block.instructions.size(), 0);
    for (size_t i = 0; i < block.instructions.size(); ++i) {
        const il::Instruction& inst = block.instructions[i];
        if (inst.transitionRunCell == il::Instruction::kNoTransitionRun) continue;
        for (size_t j = i; j < block.instructions.size() && j < i + inst.transitionRunLength; ++j) interior[j] = 1;
    }
    return interior;
}

class ClaimWriter {
public:
    ClaimWriter(il::Module& mod, const FeedbackPlan& plan, FeedbackApplyStats& stats)
        : mod_(mod), plan_(plan), stats_(stats) {
        for (uint32_t k = 0; k < mod.keyConstants.size(); ++k) keyIndex_.emplace(mod.keyConstants[k], k);
    }

    // A: one property site.
    void claim(il::Instruction& inst, const std::vector<uint8_t>& observable) {
        if (inst.staticSlot != il::Instruction::kNoStaticSlot || inst.directTarget != il::Instruction::kNoDirectTarget ||
            il::isDefineWrite(inst) || inst.icAbsent || inst.operands.empty() ||
            inst.operands[0] >= observable.size() || observable[inst.operands[0]] == 0 ||
            inst.keyIndex >= mod_.keyConstants.size() || il::parseIndexKey(mod_.keyConstants[inst.keyIndex])) {
            return;
        }
        const auto it = plan_.shapesByIc.find(inst.icIndex);
        if (it == plan_.shapesByIc.end()) return;
        const std::string* shape = it->second.dominant(kDominantPercent);
        if (shape == nullptr) return;
        std::vector<std::string_view> keys;
        bool complete = false;
        const std::string& key = mod_.keyConstants[inst.keyIndex];
        uint32_t slot = kNone;
        bool prototypeLike = false;
        if (shapeKeys(*shape, keys, complete)) {
            for (uint32_t i = 0; i < keys.size(); ++i) {
                if (keys[i] == key) slot = slot == kNone ? i : kNone - 1;
                if (keys[i] == "constructor") prototypeLike = true;
            }
        }
        const bool write = inst.op == il::Op::PropSet;
        // A write to an object others inherit from must reach the runtime,
        // which moves the prototype epoch (the runtime also refuses to publish
        // a prototype's shape for a store; `constructor` is how a prototype
        // looks from here).
        if (slot >= kNone - 1 || (write && prototypeLike)) {
            ++stats_.shapeRefused;
            return;
        }
        inst.staticSlot = slot;
        inst.staticSlotGuessed = true;
        if (claimedIcs_.insert(inst.icIndex).second) stats_.shapeExecutions += it->second.total;
        uint32_t lo = 0, span = 0;
        if (!write && complete && familyOf(keys, slot, inst.keyIndex, &lo, &span)) {
            inst.staticCellIndex = 0;
            inst.familyLo = lo;
            inst.familySpan = span;
            ++stats_.shapeFamily;
            return;
        }
        inst.staticCellIndex = mod_.staticSiteCount++;
        inst.familyLo = il::Instruction::kNoFamily;
        inst.familySpan = 0;
        ++stats_.shapeIdentity;
    }

    // C: a truthiness unbox whose every observed value was a Boolean, an
    // object, undefined or null: the kinds it saw (the Number case is
    // `hint`'s).
    void truth(il::Instruction& inst, const std::vector<uint8_t>& observable) {
        if (inst.op != il::Op::Unbox || inst.type != il::Type::Bool || inst.numberHint || inst.truthHint != 0 ||
            inst.operands.empty()) {
            return;
        }
        const il::ValueId v = inst.operands[0];
        if (v >= observable.size() || observable[v] == 0) return;
        const auto it = plan_.values.find(valueKey(inst, 0));
        if (it == plan_.values.end() || it->second.total < kMinExecutions) return;
        const NumberCounts& c = it->second;
        if (c.bools + c.objects + c.undefineds + c.nulls != c.total) return;
        uint8_t kinds = 0;
        if (c.bools) kinds |= il::Instruction::kTruthBool;
        if (c.objects) kinds |= il::Instruction::kTruthObject;
        if (c.undefineds) kinds |= il::Instruction::kTruthUndefined;
        if (c.nulls) kinds |= il::Instruction::kTruthNull;
        inst.truthHint = kinds;
        ++stats_.truthHints;
        if (hinted_.insert(it->first).second) stats_.hintExecutions += c.total;
    }

    // C: one numeric op.
    void hint(il::Instruction& inst, const std::vector<uint8_t>& observable) {
        if (inst.numberHint || !il::observesOperandTags(inst)) return;
        bool any = false;
        uint64_t executions = 0;
        for (uint32_t j = 0; j < inst.operands.size(); ++j) {
            const il::ValueId v = inst.operands[j];
            if (v >= observable.size() || observable[v] == 0) continue;
            const auto it = plan_.values.find(valueKey(inst, j));
            if (it == plan_.values.end() || it->second.total < kMinExecutions ||
                it->second.numbers != it->second.total) {
                return;
            }
            any = true;
            if (hinted_.insert(it->first).second) executions += it->second.total;
        }
        if (!any) return;
        inst.numberHint = true;
        ++stats_.numberHints;
        stats_.hintExecutions += executions;
    }

private:
    bool familyOf(const std::vector<std::string_view>& keys, uint32_t slot, uint32_t keyIndex, uint32_t* lo,
                  uint32_t* span) const {
        std::vector<uint32_t> slots;
        slots.reserve(keys.size());
        for (std::string_view k : keys) {
            const auto it = keyIndex_.find(std::string(k));
            if (it == keyIndex_.end()) return false;
            slots.push_back(it->second);
        }
        return wpo::familyClaimFor(mod_, slots, slot, keyIndex, /*write=*/false, lo, span);
    }

    il::Module& mod_;
    const FeedbackPlan& plan_;
    FeedbackApplyStats& stats_;
    std::unordered_map<std::string, uint32_t> keyIndex_;
    std::unordered_set<uint32_t> claimedIcs_;
    std::set<ValueKey> hinted_;
};

// The profile's one callee for `names`, as a module function, or kNone.
uint32_t calleeOf(const FeedbackPlan& plan, const NameCounts& names) {
    const std::string* name = names.dominant(kDominantPercent);
    if (name == nullptr) return kNone;
    const auto it = plan.callees.find(*name);
    return it == plan.callees.end() ? kNone : it->second;
}

}  // namespace

void applyFeedbackDirectMethods(il::Module& mod, const FeedbackPlan& plan,
                                const std::unordered_map<uint32_t, uint32_t>& ownCallees, FeedbackApplyStats* stats) {
    std::unordered_set<uint32_t> planned;
    for (il::Function& fn : mod.functions) {
        for (il::Block& block : fn.blocks) {
            for (il::Instruction& inst : block.instructions) {
                if (inst.op != il::Op::MethodCall || inst.operands.empty() ||
                    inst.directTarget != il::Instruction::kNoDirectTarget ||
                    (inst.immI32 & il::Instruction::kMethodOwnCallee) != 0 || ownCallees.count(inst.icIndex) != 0) {
                    continue;
                }
                const auto it = plan.methodsByIc.find(inst.icIndex);
                if (it == plan.methodsByIc.end()) continue;
                const uint32_t target = calleeOf(plan, it->second);
                if (target == kNone) continue;
                // The terms of a whole-program direct target (wpo/stamp.cpp,
                // planDirect): an entry a fixed operand list can fill. The
                // guard is the site's method cache, whose direct entry names
                // this code only for a receiver whose lookup finds it.
                const il::Function& callee = mod.functions[target];
                const size_t argc = inst.operands.size() - 1;
                bool fits = !callee.needsEnv && !callee.needsArguments && !callee.hasRestParam &&
                            !callee.isExternal && argc <= callee.callerParamCount();
                const size_t base = callee.firstSourceParam();
                for (size_t i = argc; fits && i < callee.callerParamCount(); ++i) {
                    if (callee.params[i + base].type != il::Type::Dynamic) fits = false;
                }
                if (!fits) {
                    if (stats) ++stats->methodRefused;
                    continue;
                }
                inst.directTarget = target;
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

void applyFeedbackClaimsAndHints(il::Module& mod, const FeedbackPlan& plan, FeedbackApplyStats* stats) {
    FeedbackApplyStats local;
    ClaimWriter writer(mod, plan, stats ? *stats : local);
    for (il::Function& fn : mod.functions) {
        if (fn.isExternal || fn.blocks.empty()) continue;
        const std::vector<uint8_t> observable = observableValues(fn);
        for (il::Block& block : fn.blocks) {
            const std::vector<uint8_t> interior = runInteriors(block);
            for (size_t i = 0; i < block.instructions.size(); ++i) {
                il::Instruction& inst = block.instructions[i];
                if ((inst.op == il::Op::PropGet || inst.op == il::Op::PropSet) && interior[i] == 0) {
                    writer.claim(inst, observable);
                } else {
                    writer.hint(inst, observable);
                    writer.truth(inst, observable);
                }
            }
        }
    }
}

}  // namespace bronze::spec
