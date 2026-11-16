#include "opt/speculate/evidence.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

#include "il/resume_rules.h"
#include "abi/bronze_abi.h"
#include "abi/bronze_feedback_site.h"
#include "opt/speculate/context.h"
#include "opt/speculate/policy.h"
#include "opt/core/shape_key.h"
#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

namespace {

constexpr uint32_t kTagBit(uint32_t tag) { return 1u << tag; }

// Recorded tags that say "a plain object" and nothing else: the receivers a
// shape guard can pass.
bool onlyPlainObjects(const spec::SiteFeedback& site) {
    return site.tagMask() == kTagBit(BRONZE_FEEDBACK_TAG_OBJECT);
}

// The names a site saw, when they are all real names: `*other*` is the
// folded tail of more than the recorder tracks.
bool namesComplete(const std::vector<std::pair<std::string, uint64_t>>& names) {
    if (names.empty()) return false;
    for (const auto& [name, count] : names) {
        if (name == "*other*" || count == 0) return false;
    }
    return true;
}

// The index a key constant names when it is a canonical array index below
// 2^31 ("0", "17"; not "01"): the keys the backend lowers `o[k]`'s element
// paths for (il2mir il_property_lowering.cpp index_key).
std::optional<int32_t> indexKey(const std::string& k) {
    if (k.empty() || k.size() > 10 || (k.size() > 1 && k[0] == '0')) return std::nullopt;
    int64_t v = 0;
    for (char c : k) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
    }
    if (v > INT32_MAX) return std::nullopt;
    return static_cast<int32_t>(v);
}

std::string hex8(uint32_t v) {
    char buf[9];
    std::snprintf(buf, sizeof buf, "%08x", v);
    return buf;
}

std::optional<SitePlan> planPropAdd(const std::string& toName, const std::string& key, BlockId block,
                                    uint32_t index);

}  // namespace

// The feedback sites of one function, found by the instruction they observe,
// and its values' types.
struct Evidence::SiteRefs {
    std::vector<il::FeedbackSite> sites;
    std::unordered_map<uint64_t, std::vector<uint32_t>> byInst;
    std::vector<il::Type> types;
    // Per value, the instruction defining it (null for parameters and block
    // parameters).
    std::vector<const il::Instruction*> defs;

    SiteRefs(const il::Module& module, const il::Function& fn)
        : sites(il::enumerateFeedbackSites(module, fn)), types(valueTypes(fn)), defs(fn.valueCount, nullptr) {
        for (uint32_t s = 0; s < sites.size(); ++s) {
            if (sites[s].block == il::FeedbackSite::kEntrySite) continue;
            byInst[key(sites[s].block, sites[s].inst)].push_back(s);
        }
        for (const il::Block& b : fn.blocks) {
            for (const il::Instruction& inst : b.instructions) {
                if (inst.result != il::kNoValue && inst.result < defs.size()) defs[inst.result] = &inst;
            }
        }
    }
    const il::Instruction* def(ValueId v) const { return v < defs.size() ? defs[v] : nullptr; }
    static uint64_t key(uint32_t block, uint32_t inst) { return (uint64_t{block} << 32) | inst; }
    // The sites of `kind` at the instruction, in ordinal order.
    std::vector<const il::FeedbackSite*> at(BlockId block, uint32_t inst, il::FeedbackKind kind) const {
        std::vector<const il::FeedbackSite*> out;
        auto it = byInst.find(key(block, inst));
        if (it == byInst.end()) return out;
        for (uint32_t s : it->second) {
            if (sites[s].kind == kind) out.push_back(&sites[s]);
        }
        return out;
    }
    il::Type type(ValueId v) const { return v < types.size() ? types[v] : il::Type::Void; }
};

Evidence::Evidence(const il::Module& module, const spec::TypeProfile& profile, const SpeculateOptions& options,
                   SpeculateStats& stats)
    : module_(module), profile_(profile), keys_(il::feedbackFunctionKeys(module)) {
    if (const spec::Harvest* h = profile.harvest()) {
        // A harvest is a table by IC and slow-site position: it says
        // something about this build only if that build numbered its sites as
        // this one does over the same text (docs/type-feedback.md, "Identity").
        if (h->digest == il::icLayoutDigest(module) && h->sites == module.icSiteCount) {
            harvest_ = h;
        } else {
            stats.harvestIgnored = 1;
        }
    }
    if (options.minEntries != 0) minEntries_ = options.minEntries;
    if (options.minSamples != 0) minSamples_ = options.minSamples;
}

const spec::SiteFeedback* Evidence::recorded(uint32_t fn, uint32_t ordinal) const {
    if (fn >= keys_.size() || keys_[fn].empty()) return nullptr;
    const spec::SiteFeedback* site = profile_.site(keys_[fn], ordinal);
    if (site == nullptr || site->total < kMinSiteObservations) return nullptr;
    return site;
}

const spec::HarvestSite* Evidence::harvested(uint32_t icIndex) const {
    if (harvest_ == nullptr) return nullptr;
    auto it = harvest_->ics.find(icIndex);
    if (it == harvest_->ics.end()) return nullptr;
    return &it->second;
}

bool Evidence::slowRan(uint32_t slowIndex) const {
    return harvest_ != nullptr && harvest_->slow.count(slowIndex) != 0;
}

uint64_t Evidence::slowCount(uint32_t slowIndex) const {
    if (harvest_ == nullptr) return 0;
    const auto it = harvest_->slow.find(slowIndex);
    return it != harvest_->slow.end() ? it->second : 0;
}

bool Evidence::slowArmed(uint32_t slowSite) const {
    if (harvest_ == nullptr || harvest_->slowSites != module_.slowSiteCount) return false;
    const size_t word = slowSite / 64;
    return word < harvest_->slowArmed.size() && ((harvest_->slowArmed[word] >> (slowSite % 64)) & 1u) != 0;
}

bool Evidence::isHot(uint32_t fn) const {
    if (fn < keys_.size() && !keys_[fn].empty()) {
        if (profile_.entryCount(keys_[fn]) >= minEntries_) return true;
        if (const spec::FunctionFeedback* ff = profile_.function(keys_[fn])) {
            for (const auto& [ordinal, site] : ff->sites) {
                if (site.total >= kMinSiteRunsForHot) return true;
            }
        }
    }
    if (harvest_ != nullptr) {
        auto it = harvest_->samples.find(il::functionNameHash(module_.functions[fn].name));
        if (it != harvest_->samples.end() && it->second >= minSamples_) return true;
    }
    return false;
}

std::optional<FunctionPlan> Evidence::plan(uint32_t fnIndex) const {
    const il::Function& fn = module_.functions[fnIndex];
    if (!functionMaySpeculate(fn)) return std::nullopt;
    if (!isHot(fnIndex)) return std::nullopt;
    const SiteRefs refs(module_, fn);
    FunctionPlan out;
    out.function = fnIndex;
    for (BlockId b = 0; b < fn.blocks.size(); ++b) {
        if (!mayPlaceResumePoint(fn, b)) continue;
        const auto& insts = fn.blocks[b].instructions;
        // Not the terminator: a site has an instruction after it, which an
        // exit after the site resumes in front of.
        for (uint32_t i = 0; i + 1 < insts.size(); ++i) {
            // A site's guards resume in front of it or after it.
            if (!il::canResumeBefore(fn.blocks[b], i) || !il::canResumeBefore(fn.blocks[b], i + 1)) continue;
            if (auto site = planSite(fnIndex, b, i, refs)) out.sites.push_back(std::move(*site));
        }
    }
    // TDZ reads alone make no S: they pay inside one, beside other sites.
    if (std::all_of(out.sites.begin(), out.sites.end(),
                    [](const SitePlan& p) { return p.kind == SiteKind::TdzRead; })) {
        return std::nullopt;
    }
    shareConstBounds(fn, out);
    shareShapeSets(fn, out);
    demoteUnsourcedInts(fn, out);
    return out;
}

// The dense-array test of a constant-index access covers every index up to
// its bound (`is.dense_array %a, k`), so the accesses of one receiver all
// test the largest index any of them was planned for: the first test proves
// the rest, which the redundancy pass then folds. Each of those accesses ran
// in bounds on the evidence, so a receiver that fails the largest bound would
// have left at one of them.
void Evidence::shareConstBounds(const il::Function& fn, FunctionPlan& plan) const {
    std::unordered_map<ValueId, int32_t> bound;
    auto receiver = [&](const SitePlan& p) { return fn.blocks[p.block].instructions[p.index].operands[0]; };
    for (const SitePlan& p : plan.sites) {
        if (p.constIndex == kNoConstIndex || p.viewKind != kDenseArray) continue;
        int32_t& k = bound.try_emplace(receiver(p), p.constIndex).first->second;
        k = std::max(k, p.constIndex);
    }
    for (SitePlan& p : plan.sites) {
        if (p.constIndex != kNoConstIndex && p.viewKind == kDenseArray) p.constBound = bound.at(receiver(p));
    }
}

std::optional<SitePlan> Evidence::planSite(uint32_t fnIndex, BlockId block, uint32_t index,
                                           const SiteRefs& refs) const {
    const il::Function& fn = module_.functions[fnIndex];
    const il::Instruction& inst = fn.blocks[block].instructions[index];
    std::optional<SitePlan> plan;
    const std::string* key = inst.keyIndex < module_.keyConstants.size() ? &module_.keyConstants[inst.keyIndex] : nullptr;
    switch (inst.op) {
        case il::Op::PropGet:
            if (key != nullptr && *key == "length") {
                plan = planLength(inst, block, index);
            } else if (key != nullptr && indexKey(*key)) {
                plan = planElem(inst, refs, block, index);
            } else {
                plan = planPropRead(fnIndex, inst, refs, block, index);
            }
            break;
        case il::Op::PropSet:
            if (key != nullptr && indexKey(*key)) {
                plan = planElem(inst, refs, block, index);
            } else {
                plan = planPropWrite(fnIndex, inst, refs, block, index);
            }
            break;
        case il::Op::MethodCall: plan = planMethod(fnIndex, inst, refs, block, index); break;
        case il::Op::DynamicCall: plan = planCall(fnIndex, inst, refs, block, index); break;
        case il::Op::Construct: plan = planConstruct(inst, refs, block, index); break;
        case il::Op::ElemGet:
        case il::Op::ElemSet: plan = planElem(inst, refs, block, index); break;
        // A binding fixed once initialized needs no evidence (tdz.cpp).
        case il::Op::EnvGetTdz:
            if (inst.envImmutable && inst.operands.size() == 1 && inst.type == il::Type::Dynamic) {
                plan = SitePlan{};
                plan->kind = SiteKind::TdzRead;
                plan->block = block;
                plan->index = index;
            }
            break;
        default:
            if (arithSupported(inst)) plan = planArith(fnIndex, inst, refs, block, index);
            break;
    }
    return plan;
}

// A read of an own data property: every receiver the site met was a plain
// object of one of at most kMaxChainShapes shapes, each holding the key as
// an own property (a harvest `own` way; a recorded shape whose key list has
// it).
std::optional<SitePlan> Evidence::planPropRead(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                               BlockId block, uint32_t index) const {
    if (inst.operands.size() != 1 || refs.type(inst.operands[0]) != il::Type::Dynamic) return std::nullopt;
    if (inst.keyIndex >= module_.keyConstants.size()) return std::nullopt;
    const std::string& key = module_.keyConstants[inst.keyIndex];
    if (!isNamedKey(key)) return std::nullopt;

    std::vector<std::string> names;
    if (const spec::HarvestSite* h = harvested(inst.icIndex)) {
        if (h->method || h->mega || h->ways.empty()) return std::nullopt;
        if (h->ways.front().form == "accessor") {
            if (h->full || h->uncached > kMaxUncachedReceivers) return std::nullopt;
            return planMember(*h, SiteKind::MemberGet, 0, block, index);
        }
        if (h->ways.front().form == "absent") return planMember(*h, SiteKind::MemberAbsent, 0, block, index);
        // More receivers than the ways held, or some no way could: what the
        // ways show is a sample, so the site tests slots, not shapes.
        if (h->full || h->uncached > kMaxUncachedReceivers) {
            for (const spec::HarvestWay& way : h->ways) {
                if (way.form == "own") names.push_back(way.receiver);
            }
            return planSlotKeyed(SiteKind::PropSlotRead, names, key, block, index);
        }
        for (const spec::HarvestWay& way : h->ways) {
            // `proto` (found on the chain: no constant holder to load from
            // here), `absent` and `array` stay generic.
            if (way.form != "own") return std::nullopt;
            names.push_back(way.receiver);
        }
    } else {
        const auto shapeSites = refs.at(block, index, il::FeedbackKind::Shape);
        if (shapeSites.size() != 1) return std::nullopt;
        const spec::SiteFeedback* site = recorded(fn, shapeSites.front()->ordinal);
        if (site == nullptr || !onlyPlainObjects(*site) || !namesComplete(site->shapes)) return std::nullopt;
        for (const auto& [name, count] : site->shapes) names.push_back(name);
    }
    if (names.empty() || names.size() > kMaxChainShapes) return std::nullopt;

    SitePlan plan;
    plan.kind = SiteKind::PropRead;
    plan.block = block;
    plan.index = index;
    for (const std::string& name : names) {
        const std::optional<ShapeName> shape = parseShapeName(name);
        if (!shape) return std::nullopt;
        const std::optional<int32_t> slot = ownSlot(*shape, key);
        if (!slot) return std::nullopt;  // the key is not own there: not an own read
        if (std::any_of(plan.shapes.begin(), plan.shapes.end(), [&](const ShapeSlot& s) { return s.key == name; })) {
            continue;
        }
        plan.shapes.push_back(ShapeSlot{name, *slot});
    }
    return plan;
}

// A read or assignment on receivers the ways only sample (the site met more
// shapes than its cache held, or receivers no way could hold): the slots the
// sampled shapes keep the key at as an own data property (writable, for a
// write), first seen first, when there are 1..kMaxChainShapes of them. The
// test is the shape's own entry for each slot (shape.has_own), so a shape the
// ways never saw is answered as well when it agrees.
std::optional<SitePlan> Evidence::planSlotKeyed(SiteKind kind, const std::vector<std::string>& names,
                                                const std::string& key, BlockId block, uint32_t index) const {
    SitePlan plan;
    plan.kind = kind;
    plan.block = block;
    plan.index = index;
    // A way this step cannot read (a dictionary, an accessor in the name) or
    // whose shape lacks the key as needed is one more receiver the generic arm
    // answers.
    for (const std::string& name : names) {
        const std::optional<ShapeName> shape = parseShapeName(name);
        if (!shape) continue;
        const std::optional<int32_t> slot =
            kind == SiteKind::PropSlotWrite ? writableSlot(*shape, key) : ownSlot(*shape, key);
        if (!slot) continue;
        if (std::any_of(plan.shapes.begin(), plan.shapes.end(), [&](const ShapeSlot& s) { return s.slot == *slot; })) {
            continue;
        }
        plan.shapes.push_back(ShapeSlot{std::string(), *slot});
    }
    if (plan.shapes.empty() || plan.shapes.size() > kMaxChainShapes) return std::nullopt;
    return plan;
}

// An assignment to a property every receiver already had as an own data
// property, on one to kMaxChainShapes shapes. A harvest says so with `store`
// ways (a write the cache filled for a key the receiver had); a single `own`
// way is a write that ADDED the key (a transition: planPropAdd). A recorded Shape site is
// observed before the write, so a shape whose key list has the key is a
// receiver that had it. Defines (object literals, class fields) stay generic.
std::optional<SitePlan> Evidence::planPropWrite(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                                BlockId block, uint32_t index) const {
    const int32_t mode = il::writeMode(inst);
    if (mode != il::kWriteSloppy && mode != il::kWriteStrict) return std::nullopt;
    if (inst.operands.size() != 2 || refs.type(inst.operands[0]) != il::Type::Dynamic ||
        refs.type(inst.operands[1]) != il::Type::Dynamic) {
        return std::nullopt;
    }
    if (inst.keyIndex >= module_.keyConstants.size()) return std::nullopt;
    const std::string& key = module_.keyConstants[inst.keyIndex];
    if (!isNamedKey(key)) return std::nullopt;

    std::vector<std::string> names;
    if (const spec::HarvestSite* h = harvested(inst.icIndex)) {
        if (h->method || h->ways.empty()) return std::nullopt;
        const bool sample = h->full || h->mega || h->uncached > kMaxUncachedReceivers;
        if (h->ways.front().form == "accessor") {
            if (sample) return std::nullopt;
            return planMember(*h, SiteKind::MemberSet, 1, block, index);
        }
        if (!sample && h->ways.size() == 1 && h->ways.front().form == "own") {
            return planPropAdd(h->ways.front().receiver, key, block, index);
        }
        for (const spec::HarvestWay& way : h->ways) {
            if (way.form == "store") {
                names.push_back(way.receiver);
            } else if (!sample) {
                return std::nullopt;
            }
        }
        if (sample) return planSlotKeyed(SiteKind::PropSlotWrite, names, key, block, index);
    } else {
        const auto shapeSites = refs.at(block, index, il::FeedbackKind::Shape);
        if (shapeSites.size() != 1) return std::nullopt;
        const spec::SiteFeedback* site = recorded(fn, shapeSites.front()->ordinal);
        if (site == nullptr || !onlyPlainObjects(*site)) return std::nullopt;
        names.push_back(site->soleShape());
    }
    if (names.empty() || names.size() > kMaxChainShapes) return std::nullopt;

    SitePlan plan;
    plan.kind = SiteKind::PropWrite;
    plan.block = block;
    plan.index = index;
    for (const std::string& name : names) {
        const std::optional<ShapeName> shape = parseShapeName(name);
        if (!shape) return std::nullopt;
        const std::optional<int32_t> slot = writableSlot(*shape, key);
        if (!slot) return std::nullopt;
        if (std::any_of(plan.shapes.begin(), plan.shapes.end(), [&](const ShapeSlot& s) { return s.key == name; })) {
            continue;
        }
        plan.shapes.push_back(ShapeSlot{name, *slot});
    }
    return plan;
}

namespace {

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
    for (ValueId v : inst.operands) {
        if (refs.type(v) != il::Type::Dynamic) return std::nullopt;
    }
    const std::string& key = module_.keyConstants[inst.keyIndex];
    const size_t argc = inst.operands.size() - 1;

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
                if (f == kNoFunction || !directCallFits(module_.functions[f], argc)) return std::nullopt;
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
    if (plan.callee == kNoFunction || !directCallFits(module_.functions[plan.callee], argc)) return std::nullopt;

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

// An assignment that added the key: a harvest's single `own` way for a write
// site names the shape the receiver went TO (runtime/ic_harvest.cpp), whose
// last key is the one added; the shape it came from is the same name without
// it. Only a key that lands inline: an add past the inline slots has to
// allocate the overflow block.
namespace {

std::optional<SitePlan> planPropAdd(const std::string& toName, const std::string& key, BlockId block,
                                    uint32_t index) {
    const std::optional<ShapeName> to = parseShapeName(toName);
    if (!to || to->keys.empty() || to->keys.back() != key) return std::nullopt;
    const std::optional<int32_t> slot = ownSlot(*to, key);
    if (!slot || *slot >= static_cast<int32_t>(to->inlineSlots) || *slot >= BRONZE_ABI_OBJ_OVERFLOW_SLOT) {
        return std::nullopt;
    }
    if (to->readOnly.back()) return std::nullopt;
    const std::string fromName = shapeNameWithKeys(*to, to->keys.size() - 1);
    SitePlan plan;
    plan.kind = SiteKind::PropAdd;
    plan.block = block;
    plan.index = index;
    plan.shapes.push_back(ShapeSlot{fromName, *slot});
    plan.shapes.push_back(ShapeSlot{toName, *slot});
    return plan;
}

}  // namespace

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
    if (callee == kNoFunction || !constructFits(module_.functions[callee], inst.operands.size() - 1)) {
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
    if (callee == kNoFunction || !directCallFits(module_.functions[callee], inst.operands.size() - 2)) {
        return std::nullopt;
    }
    SitePlan plan;
    plan.kind = SiteKind::Call;
    plan.block = block;
    plan.index = index;
    plan.callee = callee;
    return plan;
}

// Numbers in. A harvest's arms (evidence.h kArithArm*) say it of a dynamic
// numeric op whose helper never ran and whose Numbers arm did: each operand
// an int32 unless its double arm ran, and the result too unless its result
// arm ran. `===` and ToBoolean have arms of their own, which say "Numbers"
// and nothing about int32s. Without a harvest word, the recorded VALUE sites
// (one per Dynamic operand, in operand order, il/feedback_site.cpp) say it of
// an operand every value of which was a Number. A typed operand counts as
// its type.
std::optional<SitePlan> Evidence::planArith(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                            BlockId block, uint32_t index) const {
    const auto valueSites = refs.at(block, index, il::FeedbackKind::Value);
    bool anyRecorded = false;
    for (const il::FeedbackSite* vs : valueSites) anyRecorded = anyRecorded || recorded(fn, vs->ordinal) != nullptr;
    const ArmEvidence arms = anyRecorded ? ArmEvidence{} : armEvidence(inst);
    std::vector<uint8_t> tags;
    size_t next = 0;
    size_t boxed = 0;
    for (ValueId v : inst.operands) {
        switch (refs.type(v)) {
            case il::Type::I32: tags.push_back(kTagInt); continue;
            case il::Type::F64: tags.push_back(kTagDouble); continue;
            case il::Type::Dynamic: break;
            default: return std::nullopt;
        }
        ++boxed;
        if (arms.numbers) {
            const size_t i = tags.size();
            tags.push_back(arms.intKnown && i < 2 && !arms.operandDouble[i] ? kTagInt : kTagDouble);
            continue;
        }
        if (next >= valueSites.size() || valueSites[next]->value != v) return std::nullopt;
        const spec::SiteFeedback* site = recorded(fn, valueSites[next++]->ordinal);
        if (site == nullptr) return std::nullopt;
        const uint32_t mask = site->tagMask();
        const uint32_t numbers = kTagBit(BRONZE_FEEDBACK_TAG_I32) | kTagBit(BRONZE_FEEDBACK_TAG_F64);
        if (mask == 0 || (mask & ~numbers) != 0) return std::nullopt;
        uint8_t t = 0;
        if (mask & kTagBit(BRONZE_FEEDBACK_TAG_I32)) t |= kTagInt;
        if (mask & kTagBit(BRONZE_FEEDBACK_TAG_F64)) t |= kTagDouble;
        tags.push_back(t);
    }
    // Nothing boxed, nothing to speculate on.
    if (boxed == 0) return std::nullopt;
    const std::optional<ArithForm> form = chooseArithForm(inst, tags, arms.resultDouble);
    if (!form) return std::nullopt;
    SitePlan plan;
    plan.kind = SiteKind::Arith;
    plan.block = block;
    plan.index = index;
    plan.operandTags = std::move(tags);
    plan.form = *form;
    return plan;
}

// What a harvest's arms say of a numeric site: nothing (numbers false) unless
// the site was armed, its helper never ran and only Number arms did.
Evidence::ArmEvidence Evidence::armEvidence(const il::Instruction& inst) const {
    ArmEvidence out;
    const uint32_t site = inst.slowSite;
    if (site == il::Instruction::kNoSlowSite || !slowArmed(site) || slowRan(site)) return out;
    switch (inst.op) {
        case il::Op::StrictEq:
            for (uint32_t arm = kStrictEqArmSame; arm <= kStrictEqArmLast; ++arm) {
                const bool number = arm == kStrictEqArmSameNumber || arm == kStrictEqArmNumbers;
                if (!number && slowRan(site + arm)) return out;
            }
            out.numbers = slowRan(site + kStrictEqArmSameNumber) || slowRan(site + kStrictEqArmNumbers);
            return out;
        case il::Op::Unbox:
            if (inst.type != il::Type::Bool) return out;
            for (uint32_t arm = 1; arm <= kTruthyArmLast; ++arm) {
                if (arm != kTruthyArmNumber && slowRan(site + arm)) return out;
            }
            out.numbers = slowRan(site + kTruthyArmNumber);
            return out;
        default:
            if (!numericArms(inst)) return out;
            out.numbers = slowRan(site + kArithArmNumbers);
            out.intKnown = true;
            out.operandDouble[0] = slowRan(site + kArithArmDouble0);
            out.operandDouble[1] = slowRan(site + kArithArmDouble1);
            out.resultDouble = slowRan(site + kArithArmResultDouble);
            return out;
    }
}

// An element access the harvested build answered one way every time: the
// site was armed, its generic fallback never ran, nor (a write) its append,
// nor did a read or write of a view find its index out of bounds or a kind
// with no code; and either its Array arm ran and its typed-array arm did not,
// or the typed-array arm did, the Array arm did not, and its kind-bit arms
// name one element kind or two (il2mir il_abi.h kElemViewKinds).
// The fallback is where every other receiver, an index past the length, a
// hole and a non-index key went, so none came. `o[k]` with an index key `k`
// (a prop.get / prop.set) is the same access at a constant index.
std::optional<SitePlan> Evidence::planElem(const il::Instruction& inst, const SiteRefs& refs, BlockId block,
                                           uint32_t index) const {
    const uint32_t site = inst.slowSite;
    if (site == il::Instruction::kNoSlowSite || !slowArmed(site)) return std::nullopt;
    if (slowRan(site) || slowRan(site + kElemArmGrow) || slowRan(site + kElemArmMiss)) return std::nullopt;
    const bool array = slowRan(site + kElemArmArray);
    const uint64_t views = slowCount(site + kElemArmView);
    if (array == (views != 0)) return std::nullopt;

    SitePlan plan;
    plan.block = block;
    plan.index = index;
    plan.viewKind = kDenseArray;
    if (views != 0) {
        // One kind: every bit arm counted none or all of the views. Two
        // kinds: one bit arm counted some of them, so the kinds are the code
        // with that bit clear and with it set (two partial bits could be
        // either pair of four codes).
        uint32_t code = 0;
        int32_t partial = -1;
        const uint32_t bitArms[3] = {kElemArmKindBit0, kElemArmKindBit1, kElemArmKindBit2};
        for (uint32_t bit = 0; bit < 3; ++bit) {
            const uint64_t n = slowCount(site + bitArms[bit]);
            if (n == views) {
                code |= 1u << bit;
            } else if (n != 0) {
                if (partial >= 0) return std::nullopt;
                partial = static_cast<int32_t>(bit);
            }
        }
        plan.viewKind = kElemViewKinds[code];
        if (partial >= 0) plan.otherViewKind = kElemViewKinds[code | (1u << partial)];
    }
    const bool keyed = inst.op == il::Op::ElemGet || inst.op == il::Op::ElemSet;
    if (!keyed) {
        const std::optional<int32_t> k = indexKey(module_.keyConstants[inst.keyIndex]);
        if (!k) return std::nullopt;
        plan.constIndex = *k;
    }
    // The receiver, then (keyed) the index, then (a write) the value.
    const size_t operands = (keyed ? 2 : 1) + (inst.op == il::Op::ElemSet || inst.op == il::Op::PropSet ? 1 : 0);
    if (inst.operands.size() != operands) return std::nullopt;
    if (inst.op == il::Op::ElemGet || inst.op == il::Op::PropGet) {
        if (inst.type != il::Type::Dynamic) return std::nullopt;
        // Two kinds read into one join: both must load as a double (the float
        // kinds and Uint32) or both as an int32 (elem.cpp typedValueType).
        const auto loadsDouble = [](int32_t kind) {
            return kind == BRONZE_ABI_TA_KIND_FLOAT32 || kind == BRONZE_ABI_TA_KIND_FLOAT64 ||
                   kind == BRONZE_ABI_TA_KIND_UINT32;
        };
        if (plan.otherViewKind != kDenseArray && loadsDouble(plan.viewKind) != loadsDouble(plan.otherViewKind)) {
            return std::nullopt;
        }
        plan.kind = SiteKind::ElemRead;
        plan.numbers = array && !slowRan(site + kElemArmNoNumbers);
    } else {
        const int32_t mode = il::writeMode(inst);
        if (mode != il::kWriteSloppy && mode != il::kWriteStrict) return std::nullopt;
        plan.kind = SiteKind::ElemWrite;
    }
    for (ValueId v : inst.operands) {
        if (refs.type(v) != il::Type::Dynamic) return std::nullopt;
    }
    return plan;
}

// `x.length` the harvested build answered from an Array's header every time:
// its Array arm ran, its String and typed-array arms did not, and no other
// receiver reached the cache (the slow arm, a filled way, a shape turn).
std::optional<SitePlan> Evidence::planLength(const il::Instruction& inst, BlockId block, uint32_t index) const {
    const uint32_t site = inst.slowSite;
    if (inst.operands.size() != 1 || inst.type != il::Type::Dynamic) return std::nullopt;
    if (site == il::Instruction::kNoSlowSite || !slowArmed(site) || slowRan(site)) return std::nullopt;
    if (!slowRan(site + kLengthArmArray) || slowRan(site + kLengthArmString) || slowRan(site + kLengthArmView) ||
        slowRan(site + il::kShapeTurnArm)) {
        return std::nullopt;
    }
    if (const spec::HarvestSite* h = harvested(inst.icIndex); h != nullptr && !h->ways.empty()) return std::nullopt;
    SitePlan plan;
    plan.kind = SiteKind::ArrayLength;
    plan.block = block;
    plan.index = index;
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
        const std::string hash = hex8(il::functionNameHash(src));  // FNV-1a 32, as the runtime's fnv32
        std::string jsName;
        if (fn.nameKeyIndex < module_.keyConstants.size()) jsName = module_.keyConstants[fn.nameKeyIndex];
        if (jsName.empty()) jsName = "<anonymous>";
        put(recordedNames_, jsName + "@" + hash, f);
        put(harvestNames_, "~" + std::to_string(fn.sourceBegin) + "@" + hash, f);
    }
}

}  // namespace bronze::opt::speculate
