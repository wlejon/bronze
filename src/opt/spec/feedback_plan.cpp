#include "opt/spec/feedback_plan.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "abi/bronze_feedback_site.h"
#include "il/feedback_site.h"
#include "opt/spec/feedback_util.h"

namespace bronze::spec {

namespace {

uint32_t fnv32(std::string_view text) {
    uint32_t h = 2166136261u;
    for (unsigned char c : text) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

void addNames(NameCounts& into, const std::vector<std::pair<std::string, uint64_t>>& names, uint64_t total) {
    into.total += total;
    for (const auto& [name, n] : names) into.counts[name] += n;
}

// The callee names of the module's functions, as the recorder spells them
// (`<name>@<8 hex digits of FNV-1a over the source text>`): each name that one
// function answers. Several functions can share a text (a class's constructor
// and a method written alike); the recorded `name` decides between them, and
// a name two functions still answer resolves to neither.
std::unordered_map<std::string, uint32_t> resolveCallees(const il::Module& mod, size_t functionCount,
                                                         const std::vector<std::string>& wanted) {
    std::unordered_map<uint32_t, std::vector<uint32_t>> byHash;
    const size_t n = std::min(functionCount, mod.functions.size());
    for (uint32_t i = 0; i < n; ++i) {
        const il::Function& fn = mod.functions[i];
        if (fn.isExternal || fn.sourceEnd <= fn.sourceBegin || fn.typedOrigin != il::Function::kNoTypedOrigin ||
            fn.sourceFile >= mod.sourceTexts.size()) {
            continue;
        }
        const std::string& text = mod.sourceTexts[fn.sourceFile];
        if (fn.sourceEnd > text.size()) continue;
        byHash[fnv32(std::string_view(text).substr(fn.sourceBegin, fn.sourceEnd - fn.sourceBegin))].push_back(i);
    }
    std::unordered_map<std::string, uint32_t> out;
    for (const std::string& callee : wanted) {
        const size_t at = callee.rfind('@');
        if (at == std::string::npos || callee.size() - at != 9) continue;
        const uint32_t hash = static_cast<uint32_t>(std::strtoul(callee.c_str() + at + 1, nullptr, 16));
        const auto it = byHash.find(hash);
        if (it == byHash.end()) continue;
        if (it->second.size() == 1) {
            out[callee] = it->second.front();
            continue;
        }
        const std::string name = callee.substr(0, at);
        uint32_t found = UINT32_MAX;
        uint32_t matches = 0;
        for (uint32_t i : it->second) {
            const std::string& shown = mod.functions[i].displayName;
            const bool same = shown == name ||
                              (shown.size() > name.size() && shown.compare(shown.size() - name.size(), name.size(), name) == 0 &&
                               shown[shown.size() - name.size() - 1] == '.');
            if (same) {
                found = i;
                ++matches;
            }
        }
        if (matches == 1) out[callee] = found;
    }
    return out;
}

}  // namespace

const std::string* NameCounts::dominant(uint64_t percent) const {
    const std::pair<const std::string, uint64_t>* best = nullptr;
    for (const auto& entry : counts) {
        if (best == nullptr || entry.second > best->second) best = &entry;
    }
    if (best == nullptr || best->first == "*other*" || best->second < kMinExecutions) return nullptr;
    if (best->second * 100 < total * percent) return nullptr;
    return &best->first;
}

SpanKey spanKey(const Span& span) {
    return SpanKey{span.file, span.begin, span.end};
}

ValueKey valueKey(const il::Instruction& inst, uint32_t operand) {
    return ValueKey{spanKey(inst.span), inst.op, inst.type, operand};
}

FeedbackPlan planFeedback(const il::Module& probe, const TypeProfile& profile, size_t functionCount) {
    FeedbackPlan plan;
    plan.stats.profileFunctions = static_cast<uint32_t>(profile.functions().size());
    const std::vector<std::string> keys = il::feedbackFunctionKeys(probe);
    for (size_t f = 0; f < probe.functions.size(); ++f) {
        const il::Function& fn = probe.functions[f];
        const FunctionFeedback* ff = keys[f].empty() ? nullptr : profile.function(keys[f]);
        if (ff == nullptr) continue;
        ++plan.stats.matchedFunctions;
        const std::vector<uint8_t> observable = observableValues(fn);
        // Value sites of one instruction come in operand order; the k-th of
        // them observes the k-th observable operand.
        uint32_t lastBlock = UINT32_MAX, lastInst = UINT32_MAX, nth = 0;
        for (const il::FeedbackSite& s : il::enumerateFeedbackSites(probe, fn)) {
            if (s.block == il::FeedbackSite::kEntrySite) {
                const auto it = ff->sites.find(s.ordinal);
                if (s.kind == il::FeedbackKind::Entry && it != ff->sites.end()) plan.entries[fn.name] += it->second.total;
                continue;
            }
            const il::Instruction& inst = fn.blocks[s.block].instructions[s.inst];
            if (s.kind == il::FeedbackKind::Value) {
                if (s.block != lastBlock || s.inst != lastInst) nth = 0;
                lastBlock = s.block;
                lastInst = s.inst;
            }
            const uint32_t k = s.kind == il::FeedbackKind::Value ? nth++ : 0;
            const auto it = ff->sites.find(s.ordinal);
            if (it == ff->sites.end()) continue;
            const SiteFeedback& fb = it->second;
            switch (s.kind) {
                case il::FeedbackKind::Value: {
                    uint32_t seen = 0;
                    for (uint32_t j = 0; j < inst.operands.size(); ++j) {
                        const il::ValueId v = inst.operands[j];
                        if (v >= observable.size() || observable[v] == 0) continue;
                        if (seen++ != k) continue;
                        NumberCounts& c = plan.values[valueKey(inst, j)];
                        c.total += fb.total;
                        c.numbers += fb.tags[BRONZE_FEEDBACK_TAG_F64] + fb.tags[BRONZE_FEEDBACK_TAG_I32];
                        c.bools += fb.tags[BRONZE_FEEDBACK_TAG_BOOL];
                        c.objects += fb.tags[BRONZE_FEEDBACK_TAG_OBJECT] + fb.tags[BRONZE_FEEDBACK_TAG_ARRAY] +
                                     fb.tags[BRONZE_FEEDBACK_TAG_TYPEDARRAY] + fb.tags[BRONZE_FEEDBACK_TAG_FUNCTION];
                        c.undefineds += fb.tags[BRONZE_FEEDBACK_TAG_UNDEFINED];
                        c.nulls += fb.tags[BRONZE_FEEDBACK_TAG_NULL];
                        ++plan.stats.valueSites;
                        break;
                    }
                    break;
                }
                case il::FeedbackKind::Shape:
                    addNames(plan.shapesByIc[inst.icIndex], fb.shapes, fb.total);
                    ++plan.stats.shapeSites;
                    break;
                case il::FeedbackKind::Result:
                    if (inst.op == il::Op::PropGet) {
                        ReadResults& r = plan.readResultsByIc[inst.icIndex];
                        r.total += fb.total;
                        r.undefineds += fb.tags[BRONZE_FEEDBACK_TAG_UNDEFINED];
                    }
                    break;
                case il::FeedbackKind::Method:
                    addNames(plan.methodShapesByIc[inst.icIndex], fb.shapes, fb.total);
                    addNames(plan.methodsByIc[inst.icIndex], fb.targets, fb.total);
                    ++plan.stats.methodSites;
                    break;
                case il::FeedbackKind::Target:
                    addNames(plan.dynamicCallees[spanKey(inst.span)], fb.targets, fb.total);
                    ++plan.stats.targetSites;
                    break;
                default:
                    break;
            }
        }
    }
    std::vector<std::string> wanted;
    for (const auto& [ic, names] : plan.methodsByIc) {
        if (const std::string* d = names.dominant(kDominantPercent)) wanted.push_back(*d);
    }
    for (const auto& [span, names] : plan.dynamicCallees) {
        if (const std::string* d = names.dominant(kDominantPercent)) wanted.push_back(*d);
    }
    plan.callees = resolveCallees(probe, functionCount, wanted);
    return plan;
}

std::string feedbackReport(const FeedbackPlan& plan, const FeedbackApplyStats& stats) {
    const auto n = [](uint64_t v) { return std::to_string(v); };
    std::string out = "feedback: profile functions " + n(plan.stats.profileFunctions) + ", matched " +
                      n(plan.stats.matchedFunctions) + "; sites value " + n(plan.stats.valueSites) + ", shape " +
                      n(plan.stats.shapeSites) + ", method " + n(plan.stats.methodSites) + ", target " +
                      n(plan.stats.targetSites) + "; callee names resolved " + n(plan.callees.size()) + "\n";
    out += "feedback: A shape claims identity " + n(stats.shapeIdentity) + " (regrow " +
           n(stats.shapeRegrow) + "), family " + n(stats.shapeFamily) +
           " (dominant shape without a claimable slot " + n(stats.shapeRefused) + "); profiled executions " +
           n(stats.shapeExecutions) + "\n";
    out += "feedback: B direct method targets " + n(stats.directMethods) + " (refused " + n(stats.methodRefused) +
           "; profiled executions " + n(stats.methodExecutions) + "), closure calls split " +
           n(stats.closureCalls) + " (profiled executions " + n(stats.closureExecutions) + ")\n";
    out += "feedback: C number hints " + n(stats.numberHints) + ", truthiness hints " + n(stats.truthHints) +
           "; profiled executions " +
           n(stats.hintExecutions) + "\n";
    out += "feedback: E inline way chains " + n(stats.chainReads) + " (prototype arm " + n(stats.chainProto) +
           "; profiled executions " + n(stats.chainExecutions) + "), own-slot callees " + n(stats.ownCallees) +
           ", own-slot sites " + n(stats.ownSlotSites) + ", native-callee targets dropped " +
           n(stats.nativeTargetsDropped) + "\n";
    return out;
}

}  // namespace bronze::spec
