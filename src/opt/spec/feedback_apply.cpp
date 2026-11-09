// The consuming half of type-feedback specialization (feedback_plan.h): the
// direct targets (B) before the passes that copy bodies, the accessor targets
// (F) before guarded inlining, and the static-slot claims (A) and number
// hints (C) on the finished module.

#include <algorithm>
#include <charconv>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "abi/bronze_abi.h"
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
bool shapeKeys(const std::string& name, std::vector<std::string_view>& keys, bool& complete,
               uint32_t* inlineSlots = nullptr) {
    const size_t open = name.find('{');
    if (open == std::string::npos || name.empty() || name.back() != '}') return false;
    if (inlineSlots != nullptr) {
        // `#N` before the brace: an inline capacity other than the default.
        *inlineSlots = BRONZE_ABI_OBJ_INLINE_SLOTS;
        const size_t hash = name.rfind('#', open);
        if (hash != std::string::npos) {
            uint32_t n = 0;
            const auto [end, ec] = std::from_chars(name.data() + hash + 1, name.data() + open, n);
            if (ec != std::errc() || end != name.data() + open || n < BRONZE_ABI_OBJ_INLINE_SLOTS ||
                n > BRONZE_ABI_OBJ_OVERFLOW_SLOT) {
                return false;
            }
            *inlineSlots = n;
        }
    }
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
        uint32_t index = kNone;
        uint32_t inlineSlots = BRONZE_ABI_OBJ_INLINE_SLOTS;
        bool prototypeLike = false;
        if (shapeKeys(*shape, keys, complete, &inlineSlots)) {
            for (uint32_t i = 0; i < keys.size(); ++i) {
                if (keys[i] == key) index = index == kNone ? i : kNone - 1;
                if (keys[i] == "constructor") prototypeLike = true;
            }
        }
        // The key's position is its slot while inline; past the shape's
        // capacity the slots number from the overflow split.
        slot = index;
        if (index < kNone - 1 && index >= inlineSlots) slot = BRONZE_ABI_OBJ_OVERFLOW_SLOT + (index - inlineSlots);
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
        // A family claims position i at slot i, which a shape whose capacity
        // splits its keys earlier never has (class_family.cpp).
        if (!write && complete && slot == index && familyOf(keys, slot, inst.keyIndex, &lo, &span)) {
            inst.staticCellIndex = 0;
            inst.familyLo = lo;
            inst.familySpan = span;
            ++stats_.shapeFamily;
            return;
        }
        inst.staticCellIndex = mod_.staticSiteCount++;
        inst.familyLo = il::Instruction::kNoFamily;
        inst.familySpan = 0;
        // The run also met a shape the claimed one grew from, by one or two
        // keys: an object read here before it was finished, whose shape the
        // cell can latch first (il.h staticRegrow).
        if (complete) {
            for (const auto& [name, count] : it->second.counts) {
                (void)count;
                if (&name == shape) continue;
                std::vector<std::string_view> before;
                bool whole = false;
                if (!shapeKeys(name, before, whole) || !whole || before.size() >= keys.size() ||
                    before.size() + 2 < keys.size() ||
                    std::string_view(name).substr(0, name.find('{')) !=
                        std::string_view(*shape).substr(0, shape->find('{'))) {
                    continue;
                }
                if (std::equal(before.begin(), before.end(), keys.begin())) {
                    inst.staticRegrow = true;
                    ++stats_.shapeRegrow;
                    break;
                }
            }
        }
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

    // E: a read's inline way chain (il.h `icChain`). Every read the run met
    // with one to four shapes, each at least 1% of its executions, and
    // nothing else: a site of more shapes is the stub's (its table probe),
    // and one a static claim answers needs no chain for its one shape. The
    // prototype arm goes where the run read a value that is not `undefined`
    // through a shape whose complete key list lacks the key: a data property
    // up the chain (WebGL's constants, on the context's prototype).
    void chain(il::Instruction& inst) {
        if (inst.op != il::Op::PropGet || inst.icChain != 0 || inst.operands.empty() ||
            inst.keyIndex >= mod_.keyConstants.size()) {
            return;
        }
        const std::string& key = mod_.keyConstants[inst.keyIndex];
        if (key == "length" || il::parseIndexKey(key)) return;
        const auto it = plan_.shapesByIc.find(inst.icIndex);
        if (it == plan_.shapesByIc.end() || it->second.total < kMinExecutions) return;
        const NameCounts& names = it->second;
        // The shapes past the 1% line count toward the ways tested, not
        // toward the limit: a rare shape installed late moves the common
        // ones down a way each. A receiver that is no plain object, or a
        // dictionary, holds no way and goes to the stub.
        uint32_t significant = 0, plain = 0;
        bool lacking = false;
        uint64_t ownReads = 0, lackingReads = 0;
        std::vector<std::string_view> keys;
        for (const auto& [name, count] : names.counts) {
            const bool major = count * 100 >= names.total;
            bool complete = false;
            if (name == "*other*") {
                if (major) return;
                continue;
            }
            if (!shapeKeys(name, keys, complete)) continue;
            ++plain;
            if (!major) continue;
            if (++significant > 4) return;
            bool has = false;
            for (std::string_view k : keys) has = has || k == key;
            if (complete && !has) lacking = true;
            (complete && !has ? lackingReads : ownReads) += count;
        }
        if (significant == 0 || (plain == 1 && inst.staticSlot != il::Instruction::kNoStaticSlot)) return;
        uint8_t chain = static_cast<uint8_t>(std::min<uint32_t>(plain, 4));
        const auto rt = plan_.readResultsByIc.find(inst.icIndex);
        if (lacking && rt != plan_.readResultsByIc.end() && rt->second.undefineds < rt->second.total) {
            chain |= il::Instruction::kChainProto;
            ++stats_.chainProto;
            if (rt->second.undefineds * 2 < rt->second.total) chain |= il::Instruction::kChainProtoFirst;
        }
        // Where most reads lack the key, the answer past the own slots is the
        // one laid out in line (a three.js `object.isInstancedMesh` test).
        if (lackingReads > ownReads) chain |= il::Instruction::kChainNotOwnFirst;
        inst.icChain = chain;
        ++stats_.chainReads;
        if (chainedIcs_.insert(inst.icIndex).second) stats_.chainExecutions += names.total;
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
    std::unordered_set<uint32_t> chainedIcs_;
    std::set<ValueKey> hinted_;
};

// The profile's one callee for `names`, as a module function, or kNone.
uint32_t calleeOf(const FeedbackPlan& plan, const NameCounts& names) {
    const std::string* name = names.dominant(kDominantPercent);
    if (name == nullptr) return kNone;
    const auto it = plan.callees.find(*name);
    return it == plan.callees.end() ? kNone : it->second;
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

}  // namespace

void applyFeedbackDirectMethods(il::Module& mod, const FeedbackPlan& plan, std::unordered_map<uint32_t, uint32_t>& ownCallees,
                                FeedbackApplyStats* stats) {
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
                const uint32_t target = calleeOf(plan, it->second);
                if (target == kNone) {
                    // Receivers that hold the key in an own slot, with no one
                    // closure in it: the SLOT form is still dispatched at the
                    // site, entering whichever closure the slot holds.
                    if (home == KeyHome::Own && (inst.immI32 & il::Instruction::kMethodOwnCallee) == 0) {
                        inst.immI32 |= il::Instruction::kMethodOwnCallee;
                        if (stats) ++stats->ownSlotSites;
                    }
                    continue;
                }
                const bool predicted = ownCallees.count(inst.icIndex) != 0;
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
                const bool own_hint = (inst.immI32 & il::Instruction::kMethodOwnCallee) != 0;
                if ((predicted || own_hint) && home != KeyHome::Inherited) continue;
                // The terms of a whole-program direct target (wpo/stamp.cpp,
                // planDirect): an entry a fixed operand list can fill. The
                // guard is the site's method cache, whose direct entry names
                // this code only for a receiver whose lookup finds it.
                // Arguments past the parameters are dropped by the direct
                // edge: with no `arguments` object and no rest parameter the
                // callee cannot observe them, and they are evaluated already.
                const il::Function& callee = mod.functions[target];
                const size_t argc = inst.operands.size() - 1;
                bool fits = !callee.needsEnv && !callee.needsArguments && !callee.hasRestParam && !callee.isExternal;
                const size_t base = callee.firstSourceParam();
                for (size_t i = argc; fits && i < callee.callerParamCount(); ++i) {
                    if (callee.params[i + base].type != il::Type::Dynamic) fits = false;
                }
                if (!fits) {
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

void applyFeedbackAccessors(il::Module& mod, const FeedbackPlan& plan, const AccessorResolver& resolve,
                            FeedbackApplyStats* stats) {
    FeedbackApplyStats local;
    FeedbackApplyStats& st = stats ? *stats : local;
    // One cell pair per site, shared by its copies as they share its cache
    // (direct_method_table.cpp, resolveAccessors).
    std::unordered_map<uint32_t, uint32_t> cellOf;
    std::unordered_set<uint32_t> refused;
    std::vector<std::string_view> keys;
    for (il::Function& fn : mod.functions) {
        if (fn.isExternal || fn.blocks.empty()) continue;
        const std::vector<uint8_t> observable = observableValues(fn);
        for (il::Block& block : fn.blocks) {
            for (il::Instruction& inst : block.instructions) {
                if (inst.op != il::Op::PropGet && inst.op != il::Op::PropSet) continue;
                // A definition never reaches a setter.
                if (inst.staticSlot != il::Instruction::kNoStaticSlot ||
                    inst.directTarget != il::Instruction::kNoDirectTarget || il::isDefineWrite(inst) ||
                    inst.operands.empty() || inst.operands[0] >= observable.size() ||
                    observable[inst.operands[0]] == 0 || inst.keyIndex >= mod.keyConstants.size()) {
                    continue;
                }
                const std::string& key = mod.keyConstants[inst.keyIndex];
                if (il::parseIndexKey(key)) continue;
                const auto it = plan.shapesByIc.find(inst.icIndex);
                if (it == plan.shapesByIc.end()) continue;
                const std::string* shape = it->second.dominant(kDominantPercent);
                bool complete = false;
                if (shape == nullptr || !shapeKeys(*shape, keys, complete) || !complete) continue;
                // The key own on the receiver is A's and E's (a slot), never
                // an accessor of the prototype's.
                if (std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
                // The class the prototype's constructor names; `^Name` (an
                // `Object.create` link in between) names none.
                std::string proto = shape->substr(0, shape->find('{'));
                if (const size_t hash = proto.find('#'); hash != std::string::npos) proto.resize(hash);
                if (proto.empty() || proto.front() == '^') continue;
                const bool setter = inst.op == il::Op::PropSet;
                const uint32_t target = resolve(proto, key, setter);
                if (target >= mod.functions.size()) {
                    if (refused.insert(inst.icIndex).second) ++st.accessorNone;
                    continue;
                }
                auto cell = cellOf.find(inst.icIndex);
                if (cell == cellOf.end()) {
                    cell = cellOf.emplace(inst.icIndex, mod.staticSiteCount).first;
                    mod.staticSiteCount += 2;
                    st.accessorExecutions += it->second.total;
                }
                inst.directTarget = target;
                inst.staticCellIndex = cell->second;
                ++(setter ? st.accessorSets : st.accessorGets);
            }
        }
    }
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
                    writer.chain(inst);
                } else {
                    writer.hint(inst, observable);
                    writer.truth(inst, observable);
                }
            }
        }
    }
}

}  // namespace bronze::spec
