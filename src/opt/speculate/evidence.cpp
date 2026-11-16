#include "opt/speculate/evidence.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

#include "il/resume_rules.h"
#include "abi/bronze_feedback_site.h"
#include "opt/speculate/context.h"
#include "opt/speculate/policy.h"
#include "opt/speculate/shape_key.h"
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

std::string hex8(uint32_t v) {
    char buf[9];
    std::snprintf(buf, sizeof buf, "%08x", v);
    return buf;
}

}  // namespace

// The feedback sites of one function, found by the instruction they observe,
// and its values' types.
struct Evidence::SiteRefs {
    std::vector<il::FeedbackSite> sites;
    std::unordered_map<uint64_t, std::vector<uint32_t>> byInst;
    std::vector<il::Type> types;

    SiteRefs(const il::Module& module, const il::Function& fn)
        : sites(il::enumerateFeedbackSites(module, fn)), types(valueTypes(fn)) {
        for (uint32_t s = 0; s < sites.size(); ++s) {
            if (sites[s].block == il::FeedbackSite::kEntrySite) continue;
            byInst[key(sites[s].block, sites[s].inst)].push_back(s);
        }
    }
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
    if (out.sites.empty()) return std::nullopt;
    return out;
}

std::optional<SitePlan> Evidence::planSite(uint32_t fnIndex, BlockId block, uint32_t index,
                                           const SiteRefs& refs) const {
    const il::Function& fn = module_.functions[fnIndex];
    const il::Instruction& inst = fn.blocks[block].instructions[index];
    std::optional<SitePlan> plan;
    switch (inst.op) {
        case il::Op::PropGet: plan = planPropRead(fnIndex, inst, refs, block, index); break;
        case il::Op::PropSet: plan = planPropWrite(fnIndex, inst, refs, block, index); break;
        case il::Op::MethodCall: plan = planMethod(fnIndex, inst, refs, block, index); break;
        case il::Op::DynamicCall: plan = planCall(fnIndex, inst, refs, block, index); break;
        case il::Op::ElemGet:
        case il::Op::ElemSet: plan = planElem(inst, refs, block, index); break;
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
        if (h->method || h->full || h->mega || h->uncached > kMaxUncachedReceivers || h->ways.empty()) {
            return std::nullopt;
        }
        for (const spec::HarvestWay& way : h->ways) {
            // `proto` (found on the chain: no constant holder to load from
            // here), `absent`, `accessor` and `array` stay generic.
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

// An assignment to a property every receiver already had as an own data
// property, on one shape. A harvest says so with a `store` way (a write the
// cache filled for a key the receiver had); its `own` way is a write that
// ADDED the key (a transition), which stays generic. A recorded Shape site is
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

    std::string name;
    if (const spec::HarvestSite* h = harvested(inst.icIndex)) {
        if (h->method || h->full || h->mega || h->uncached > kMaxUncachedReceivers || h->ways.size() != 1 ||
            h->ways.front().form != "store") {
            return std::nullopt;
        }
        name = h->ways.front().receiver;
    } else {
        const auto shapeSites = refs.at(block, index, il::FeedbackKind::Shape);
        if (shapeSites.size() != 1) return std::nullopt;
        const spec::SiteFeedback* site = recorded(fn, shapeSites.front()->ordinal);
        if (site == nullptr || !onlyPlainObjects(*site)) return std::nullopt;
        name = site->soleShape();
    }
    const std::optional<ShapeName> shape = parseShapeName(name);
    if (!shape) return std::nullopt;
    const std::optional<int32_t> slot = ownSlot(*shape, key);
    if (!slot) return std::nullopt;

    SitePlan plan;
    plan.kind = SiteKind::PropWrite;
    plan.block = block;
    plan.index = index;
    plan.shapes.push_back(ShapeSlot{name, *slot});
    return plan;
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
        std::string callee;
        bool anySlot = false;
        for (const spec::HarvestWay& way : h->ways) {
            if (way.form != "direct" && way.form != "slot") return std::nullopt;
            if (way.callee == "*other*" || (!callee.empty() && way.callee != callee)) return std::nullopt;
            callee = way.callee;
            anySlot = anySlot || way.form == "slot";
        }
        plan.callee = calleeByHarvestName(callee);
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

// A plain call whose callee was always one function: a recorded Target
// site's, or a harvest's callee turns (il/feedback_site.h kLastShapeArm) —
// the site's code entered one callee directly, once it first changed from
// none, and its helper never ran.
std::optional<SitePlan> Evidence::planCall(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                           BlockId block, uint32_t index) const {
    if (inst.operands.size() < 2) return std::nullopt;
    for (ValueId v : inst.operands) {
        if (refs.type(v) != il::Type::Dynamic) return std::nullopt;
    }
    uint32_t callee = kNoFunction;
    const auto targetSites = refs.at(block, index, il::FeedbackKind::Target);
    if (targetSites.size() == 1) {
        const spec::SiteFeedback* site = recorded(fn, targetSites.front()->ordinal);
        if (site == nullptr || site->tagMask() != kTagBit(BRONZE_FEEDBACK_TAG_FUNCTION) ||
            !namesComplete(site->targets)) {
            return std::nullopt;
        }
        callee = calleeByRecordedName(site->soleTarget());
    } else if (inst.slowSite != il::Instruction::kNoSlowSite && slowArmed(inst.slowSite + il::kShapeTurnArm)) {
        const auto turns = harvest_->slow.find(inst.slowSite + il::kShapeTurnArm);
        const auto last = harvest_->callees.find(inst.slowSite + il::kLastShapeArm);
        if (slowRan(inst.slowSite) || turns == harvest_->slow.end() || turns->second != 1 ||
            last == harvest_->callees.end()) {
            return std::nullopt;
        }
        callee = calleeByHarvestName(last->second);
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

// Numbers in, from the recorded VALUE sites: one per Dynamic operand, in
// operand order (il/feedback_site.cpp). An operand qualifies when every value
// it held was a Number; a typed operand counts as its type. With no recorded
// site, a harvest's arms say the same of a `+ - * /` whose helper never ran
// and whose Numbers arm did: each operand an int32 unless its double arm ran.
std::optional<SitePlan> Evidence::planArith(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                            BlockId block, uint32_t index) const {
    const auto valueSites = refs.at(block, index, il::FeedbackKind::Value);
    const bool armed = valueSites.empty() && inst.operands.size() == 2 &&
                       inst.slowSite != il::Instruction::kNoSlowSite && slowArmed(inst.slowSite) &&
                       !slowRan(inst.slowSite) && slowRan(inst.slowSite + kArithArmNumbers);
    std::vector<uint8_t> tags;
    size_t next = 0;
    for (ValueId v : inst.operands) {
        switch (refs.type(v)) {
            case il::Type::I32: tags.push_back(kTagInt); continue;
            case il::Type::F64: tags.push_back(kTagDouble); continue;
            case il::Type::Dynamic: break;
            default: return std::nullopt;
        }
        if (armed) {
            const uint32_t arm = tags.empty() ? kArithArmDouble0 : kArithArmDouble1;
            tags.push_back(slowRan(inst.slowSite + arm) ? kTagDouble : kTagInt);
            ++next;
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
    if (next == 0) return std::nullopt;
    const std::optional<ArithForm> form = chooseArithForm(inst, tags);
    if (!form) return std::nullopt;
    SitePlan plan;
    plan.kind = SiteKind::Arith;
    plan.block = block;
    plan.index = index;
    plan.operandTags = std::move(tags);
    plan.form = *form;
    return plan;
}

// An element access the harvested build answered from an Array every time:
// the site was armed, its generic fallback never ran, its Array arm ran and
// its typed-array arm did not, nor (a write) its append. The fallback is where
// every other receiver, an index past the length, a hole and a non-index key
// went, so none came.
std::optional<SitePlan> Evidence::planElem(const il::Instruction& inst, const SiteRefs& refs, BlockId block,
                                           uint32_t index) const {
    if (inst.slowSite == il::Instruction::kNoSlowSite || !slowArmed(inst.slowSite)) return std::nullopt;
    if (slowRan(inst.slowSite) || !slowRan(inst.slowSite + kElemArmArray) || slowRan(inst.slowSite + kElemArmView) ||
        slowRan(inst.slowSite + kElemArmGrow)) {
        return std::nullopt;
    }
    SitePlan plan;
    plan.block = block;
    plan.index = index;
    if (inst.op == il::Op::ElemGet) {
        if (inst.operands.size() != 2 || inst.type != il::Type::Dynamic) return std::nullopt;
        plan.kind = SiteKind::ElemRead;
    } else {
        const int32_t mode = il::writeMode(inst);
        if (inst.operands.size() != 3 || (mode != il::kWriteSloppy && mode != il::kWriteStrict)) return std::nullopt;
        plan.kind = SiteKind::ElemWrite;
    }
    for (ValueId v : inst.operands) {
        if (refs.type(v) != il::Type::Dynamic) return std::nullopt;
    }
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
