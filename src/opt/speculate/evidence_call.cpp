// CALLS AND `new` (evidence.h): a method site's member or Math intrinsic,
// the callee a harvested call or construct entered, and the names the
// recorder and the harvest give callees.

#include "opt/speculate/evidence.h"
#include "opt/speculate/evidence_site_refs.h"

#include <cstdio>
#include <unordered_map>

#include "abi/bronze_abi.h"
#include "abi/bronze_feedback_site.h"
#include "opt/speculate/context.h"
#include "opt/speculate/policy.h"
#include "opt/core/shape_key.h"
#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

namespace {

std::string hex8(uint32_t v) {
    char buf[9];
    std::snprintf(buf, sizeof buf, "%08x", v);
    return buf;
}

// The il::Intrinsic a harvest's callee name `intrinsic:Math.<name>` names,
// when its computed form takes `argc` arguments; -1 otherwise.
int32_t intrinsicNamed(const std::string& callee, size_t argc) {
    static const std::string kPrefix = "intrinsic:Math.";
    if (callee.compare(0, kPrefix.size(), kPrefix) != 0) return -1;
    for (int32_t i = 0; i < il::kIntrinsicCount; ++i) {
        const il::IntrinsicInfo* info = il::intrinsicInfo(i);
        if (kPrefix + info->name != callee) continue;
        return argc == info->arity ? i : -1;
    }
    return -1;
}

SitePlan mathCallPlan(int32_t intrinsic, BlockId block, uint32_t index) {
    SitePlan plan;
    plan.kind = SiteKind::MathCall;
    plan.block = block;
    plan.index = index;
    plan.intrinsic = intrinsic;
    return plan;
}

}  // namespace

// `Math.<key>` with Math pinned (il::Module::mathPinned): `global.get "Math"`
// is the frozen builtin, whose `<key>` is a non-writable, non-configurable
// data property holding the intrinsic, so the read can only answer it.
int32_t Evidence::pinnedMathIntrinsic(ValueId recv, uint32_t keyIndex, size_t argc, const SiteRefs& refs) const {
    if (!module_.mathPinned || keyIndex >= module_.keyConstants.size()) return -1;
    const il::Instruction* d = refs.def(recv);
    if (d == nullptr || d->op != il::Op::GlobalGet || d->keyIndex >= module_.keyConstants.size() ||
        module_.keyConstants[d->keyIndex] != "Math") {
        return -1;
    }
    return intrinsicNamed("intrinsic:Math." + module_.keyConstants[keyIndex], argc);
}

// A method call with one callee. A harvest names it per way (`direct`: found
// on the chain, `slot`: an own slot); a recorded Method site names the
// receiver's shapes and the function the key resolved to. One own-slot shape
// gives MethodSlot (the method is loaded from the slot); anything else with
// one callee gives MethodProto (the method is fetched generically and its
// identity tested), which holds whatever the receivers were.
std::optional<SitePlan> Evidence::planMethod(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                             BlockId block, uint32_t index) const {
    if (inst.operands.empty() || refs.type(inst.operands[0]) != il::Type::Dynamic) return std::nullopt;
    if (inst.keyIndex >= module_.keyConstants.size()) return std::nullopt;
    if (const int32_t m = pinnedMathIntrinsic(inst.operands[0], inst.keyIndex, inst.operands.size() - 1, refs);
        m >= 0) {
        SitePlan math = mathCallPlan(m, block, index);
        math.pinned = true;
        return math;
    }
    // A collection's get / has takes its key boxed (collection_call.cpp), so
    // it is planned whatever the key's type.
    if (const spec::HarvestSite* h = harvested(inst.icIndex); h != nullptr && h->method && !h->mega) {
        if (std::optional<SitePlan> coll = planCollection(*h, module_.keyConstants[inst.keyIndex], inst.operands.size() - 1, block, index)) {
            return coll;
        }
    }
    for (ValueId v : inst.operands) {
        if (refs.type(v) != il::Type::Dynamic) return std::nullopt;
    }
    const std::string& key = module_.keyConstants[inst.keyIndex];
    const size_t argc = inst.operands.size() - 1;
    // `a.push(v)` on an array this function made (array_push.cpp): the
    // allocation is the evidence, the receiver's shape is tested.
    if (key == "push" && argc == 1) {
        const il::Instruction* d = refs.def(refs.single(inst.operands[0]));
        if (d != nullptr && (d->op == il::Op::ArrayLiteral || d->op == il::Op::CreateArray)) {
            SitePlan push;
            push.kind = SiteKind::ArrayPush;
            push.block = block;
            push.index = index;
            return push;
        }
    }

    SitePlan plan;
    plan.block = block;
    plan.index = index;
    std::string ownShape;  // set when one own-slot shape holds the method
    if (const spec::HarvestSite* h = harvested(inst.icIndex)) {
        if (!h->method || h->mega || h->uncached > kMaxUncachedReceivers || h->ways.empty()) return std::nullopt;
        if (std::optional<SitePlan> math = planIntrinsic(*h, argc, block, index)) return math;
        if (std::optional<SitePlan> member = planMember(*h, SiteKind::MemberMethod, argc, block, index)) {
            return member;
        }
        std::vector<std::string> callees;
        bool anySlot = false;
        for (const spec::HarvestWay& way : h->ways) {
            if (way.form != "direct" && way.form != "slot") return std::nullopt;
            if (way.callee == "*other*") return std::nullopt;
            if (std::find(callees.begin(), callees.end(), way.callee) == callees.end()) callees.push_back(way.callee);
            anySlot = anySlot || way.form == "slot";
        }
        if (callees.size() > kMaxChainShapes) return std::nullopt;
        if (callees.size() > 1) {
            // Several callees: the method is fetched as G fetches it and its
            // identity picks the arm (method_call.cpp).
            for (const std::string& name : callees) {
                const uint32_t f = calleeByHarvestName(name);
                if (f == kNoFunction || !directCallFits(module_, module_.functions[f], argc)) return std::nullopt;
                plan.callees.push_back(f);
            }
            plan.kind = SiteKind::MethodProto;
            plan.calleesOpen = anySlot;
            return plan;
        }
        plan.callee = calleeByHarvestName(callees.front());
        plan.calleesOpen = anySlot;
        if (anySlot && h->ways.size() == 1) ownShape = h->ways.front().receiver;
    } else {
        const auto methodSites = refs.at(block, index, il::FeedbackKind::Method);
        if (methodSites.size() != 1) return std::nullopt;
        const spec::SiteFeedback* site = recorded(fn, methodSites.front()->ordinal);
        if (site == nullptr || !namesComplete(site->targets)) return std::nullopt;
        const std::string target = site->soleTarget();
        if (target.empty()) return std::nullopt;
        plan.callee = calleeByRecordedName(target);
        const std::string shape = site->soleShape();
        if (onlyPlainObjects(*site) && !shape.empty()) {
            if (auto parsed = parseShapeName(shape); parsed && ownSlot(*parsed, key)) ownShape = shape;
        }
    }
    if (plan.callee == kNoFunction || !directCallFits(module_, module_.functions[plan.callee], argc)) return std::nullopt;

    if (!ownShape.empty() && isNamedKey(key)) {
        const std::optional<ShapeName> shape = parseShapeName(ownShape);
        const std::optional<int32_t> slot = shape ? ownSlot(*shape, key) : std::nullopt;
        if (slot) {
            plan.kind = SiteKind::MethodSlot;
            plan.shapes.push_back(ShapeSlot{ownShape, *slot});
            return plan;
        }
    }
    plan.kind = SiteKind::MethodProto;
    return plan;
}

// A method call whose every way's callee was one Math function the runtime
// exports (il::Intrinsic; ic_harvest.cpp names it `intrinsic:Math.<name>`),
// called with exactly the arguments its computed form takes.
std::optional<SitePlan> Evidence::planIntrinsic(const spec::HarvestSite& h, size_t argc, BlockId block,
                                                uint32_t index) const {
    std::string name;
    for (const spec::HarvestWay& way : h.ways) {
        if (!name.empty() && way.callee != name) return std::nullopt;
        name = way.callee;
    }
    const int32_t intrinsic = intrinsicNamed(name, argc);
    if (intrinsic < 0) return std::nullopt;
    return mathCallPlan(intrinsic, block, index);
}

// The callee a harvested call or `new` entered (il/feedback_site.h
// kLastShapeArm): its code entered one callee directly, once it first
// changed from none, and its helper never ran.
//
// A `new` also takes its helper whenever the allocation window has no room
// for the instance, and before the first construction of a constructor vets
// it (bronze_construct), so for one the helper's count says nothing; the
// turns alone name the callee.
const std::string* Evidence::harvestedCalleeName(const il::Instruction& inst) const {
    if (harvest_ == nullptr || inst.slowSite == il::Instruction::kNoSlowSite ||
        !slowArmed(inst.slowSite + il::kShapeTurnArm)) {
        return nullptr;
    }
    const auto turns = harvest_->slow.find(inst.slowSite + il::kShapeTurnArm);
    const auto last = harvest_->callees.find(inst.slowSite + il::kLastShapeArm);
    if (turns == harvest_->slow.end() || turns->second != 1 || last == harvest_->callees.end()) return nullptr;
    return &last->second;
}

uint32_t Evidence::harvestedCallee(const il::Instruction& inst) const {
    if (inst.op != il::Op::Construct && inst.slowSite != il::Instruction::kNoSlowSite && slowRan(inst.slowSite)) {
        return kNoFunction;
    }
    const std::string* name = harvestedCalleeName(inst);
    return name != nullptr ? calleeByHarvestName(*name) : kNoFunction;
}

// A `new` whose callee was always one function, entered inline (G's
// construct fast path, il_call_entry.cpp: vetted, its instance made from its
// instance shape), whose convention a direct call with `this` fits.
std::optional<SitePlan> Evidence::planConstruct(const il::Instruction& inst, const SiteRefs& refs, BlockId block,
                                                uint32_t index) const {
    if (inst.operands.empty()) return std::nullopt;
    for (ValueId v : inst.operands) {
        if (refs.type(v) != il::Type::Dynamic) return std::nullopt;
    }
    const uint32_t callee = harvestedCallee(inst);
    if (callee == kNoFunction || !constructFits(module_, module_.functions[callee], inst.operands.size() - 1)) {
        return std::nullopt;
    }
    SitePlan plan;
    plan.kind = SiteKind::Construct;
    plan.block = block;
    plan.index = index;
    plan.callee = callee;
    return plan;
}

// A plain call whose callee was always one function: a recorded Target
// site's, or a harvest's callee turns (il/feedback_site.h kLastShapeArm) —
// the site's code entered one callee directly, once it first changed from
// none, and its helper never ran.
std::optional<SitePlan> Evidence::planCall(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                           BlockId block, uint32_t index) const {
    if (inst.operands.size() < 2) return std::nullopt;
    // `const cos = Math.cos; cos(x)` with Math pinned: the callee is the
    // read of the intrinsic.
    if (const il::Instruction* read = refs.def(inst.operands[0]);
        read != nullptr && read->op == il::Op::PropGet && read->operands.size() == 1) {
        const int32_t m = pinnedMathIntrinsic(read->operands[0], read->keyIndex, inst.operands.size() - 2, refs);
        if (m >= 0) {
            SitePlan math = mathCallPlan(m, block, index);
            math.pinned = true;
            return math;
        }
    }
    for (ValueId v : inst.operands) {
        if (refs.type(v) != il::Type::Dynamic) return std::nullopt;
    }
    uint32_t callee = kNoFunction;
    const auto targetSites = refs.at(block, index, il::FeedbackKind::Target);
    const spec::SiteFeedback* site = targetSites.size() == 1 ? recorded(fn, targetSites.front()->ordinal) : nullptr;
    if (site != nullptr) {
        if (site->tagMask() != kTagBit(BRONZE_FEEDBACK_TAG_FUNCTION) ||
            !namesComplete(site->targets)) {
            return std::nullopt;
        }
        callee = calleeByRecordedName(site->soleTarget());
    } else {
        // A Math function held in a variable (`const cos = Math.cos`).
        if (const std::string* name = harvestedCalleeName(inst)) {
            const int32_t intrinsic = intrinsicNamed(*name, inst.operands.size() - 2);
            if (intrinsic >= 0) return mathCallPlan(intrinsic, block, index);
        }
        callee = harvestedCallee(inst);
    }
    if (callee == kNoFunction || !directCallFits(module_, module_.functions[callee], inst.operands.size() - 2)) {
        return std::nullopt;
    }
    SitePlan plan;
    plan.kind = SiteKind::Call;
    plan.block = block;
    plan.index = index;
    plan.callee = callee;
    return plan;
}

std::string Evidence::sourceText(const il::Function& fn) const {
    if (fn.sourceEnd == 0 || fn.sourceFile >= module_.sourceTexts.size()) return {};
    const std::string& text = module_.sourceTexts[fn.sourceFile];
    if (fn.sourceEnd > text.size() || fn.sourceBegin > fn.sourceEnd) return {};
    return text.substr(fn.sourceBegin, fn.sourceEnd - fn.sourceBegin);
}

// The recorder names a callee `<name>@<fnv32 of its source, 8 hex>` and the
// harvest `~<source offset>@<fnv32>` (runtime/type_feedback.cpp calleeText,
// typeFeedbackCodeName). A name matching two functions (one source range
// lowered twice) names neither.
uint32_t Evidence::calleeByRecordedName(const std::string& name) const {
    buildCalleeNames();
    auto it = recordedNames_.find(name);
    return it == recordedNames_.end() ? kNoFunction : it->second;
}

uint32_t Evidence::calleeByHarvestName(const std::string& name) const {
    buildCalleeNames();
    auto it = harvestNames_.find(name);
    return it == harvestNames_.end() ? kNoFunction : it->second;
}

void Evidence::buildCalleeNames() const {
    if (namesBuilt_) return;
    namesBuilt_ = true;
    auto put = [](std::unordered_map<std::string, uint32_t>& map, std::string key, uint32_t fn) {
        auto [it, fresh] = map.emplace(std::move(key), fn);
        if (!fresh) it->second = kNoFunction;
    };
    for (uint32_t f = 0; f < module_.functions.size(); ++f) {
        const il::Function& fn = module_.functions[f];
        if (fn.isExternal || fn.isSpeculated() || fn.isCoroutineBody()) continue;
        const std::string src = sourceText(fn);
        if (src.empty()) continue;
        const std::string hash = hex8(il::fnv32(src));
        std::string jsName;
        if (fn.nameKeyIndex < module_.keyConstants.size()) jsName = module_.keyConstants[fn.nameKeyIndex];
        if (jsName.empty()) jsName = "<anonymous>";
        put(recordedNames_, jsName + "@" + hash, f);
        put(harvestNames_, "~" + std::to_string(fn.sourceBegin) + "@" + hash, f);
    }
}

}  // namespace bronze::opt::speculate
