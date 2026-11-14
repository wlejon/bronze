#include "opt/spec/feedback_plan.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

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
        // A harvest names a callee by where its text starts (`~<offset>`):
        // the cache holds its code, not the function object with its name.
        const bool byOffset = !name.empty() && name.front() == '~';
        const uint32_t offset = byOffset ? static_cast<uint32_t>(std::strtoul(name.c_str() + 1, nullptr, 10)) : 0;
        uint32_t found = UINT32_MAX;
        uint32_t matches = 0;
        for (uint32_t i : it->second) {
            const std::string& shown = mod.functions[i].displayName;
            const bool same = byOffset ? mod.functions[i].sourceBegin == offset
                                       : shown == name || (shown.size() > name.size() &&
                                                           shown.compare(shown.size() - name.size(), name.size(), name) == 0 &&
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

// Whether shape name `grown` is `from` with one or two more keys: an object
// met before it was finished and after.
bool grewFrom(const std::string& grown, const std::string& from) {
    if (from.size() < 2 || from.back() != '}' || grown.size() <= from.size() || grown.back() != '}') return false;
    const size_t stem = from.size() - 1;
    if (grown.compare(0, stem, from, 0, stem) != 0 || grown[stem] != ',') return false;
    const auto more = std::count(grown.begin() + static_cast<std::ptrdiff_t>(stem), grown.end(), ',');
    return more <= 2;
}

// Whether `key` is the last key a shape name lists (`P{...,key}`): a write
// cache holding that shape may have latched the transition that ADDS the key,
// whose receivers have the shape one key short (object.cpp, the
// shape-transition hit), or a store to a key the receiver already had.
bool lastKeyIs(const std::string& shape, const std::string& key) {
    if (key.empty() || shape.size() < key.size() + 2 || shape.back() != '}') return false;
    const size_t start = shape.size() - 1 - key.size();
    if (shape.compare(start, key.size(), key) != 0) return false;
    return shape[start - 1] == ',' || shape[start - 1] == '{';
}

// HARVEST (docs/type-feedback.md, "Harvest"): a cache's contents filed as a
// recorded site's would be. A cache keeps what a site met, never how often,
// so every harvested site stands for kHarvestExecutions (past every
// threshold: a site whose cache filled ran at least once, and nothing says
// how much more), split evenly over the receivers it held. Where the cache
// says it met more than it holds, half goes to `*other*`, which no consumer
// claims and which keeps a read off the inline way chain (E):
//   - a read whose four ways were all in use (move to front: at least four
//     shapes, the stub's table answering the rest);
//   - a write whose way-0 misses the table answered (it keeps one way);
//   - a method call whose stub's table answered;
//   - a site whose `other` row says it met, at least kHarvestUncachedMin
//     times, a receiver no way could hold (an Array, a primitive, a
//     dictionary), which a claim's guard would send to its resume twin.
// So one receiver held and nothing more is a dominant shape (A, F), two to
// three a chain (E), two at a method site poly receivers (B). A write way
// filled by the add that made its key (form `own`, the key the shape's last)
// may have met only receivers one key short, and gives that receiver's share
// half to `*other*`: no claim either way; a way a store filled (`store`) met
// the shape it holds. A write that met a shape and the one it grew from by a
// key or two (a setter run while the object was being built, and after)
// counts as the grown shape's, the other keeping one execution for the
// claim's regrow (feedback_apply.cpp): a write has no chain, so shared evenly
// neither is claimed and every store takes the stub. A read's answers are what its ways
// say: `absent` answered undefined, everything else a value.
void planHarvest(const il::Module& probe, const Harvest& harvest, FeedbackPlan& plan) {
    plan.stats.harvest = true;
    if (harvest.digest != probe.icLayoutDigest || harvest.sites != probe.icSiteCount) {
        plan.stats.harvestStale = true;
        return;
    }
    plan.harvestDoubles = harvest.doubles;
    plan.mixedKeys = harvest.mixed;
    plan.harvestObjects = harvest.objects;
    plan.nonObjectKeys = harvest.nonObject;
    if (harvest.slowSites != 0 && harvest.slowSites == probe.slowSiteCount) {
        for (uint32_t i = 0; i < harvest.slowSites; ++i) {
            const bool armed = i / 64 < harvest.slowArmed.size() && (harvest.slowArmed[i / 64] >> (i % 64) & 1) != 0;
            if (armed && harvest.slow.count(i) == 0) plan.slowCold.insert(i);
        }
    }
    struct Named {
        il::Op op;
        uint32_t key;
    };
    std::unordered_map<uint32_t, Named> named;
    for (const il::Function& fn : probe.functions) {
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (inst.op == il::Op::PropGet || inst.op == il::Op::PropSet || inst.op == il::Op::MethodCall ||
                    inst.op == il::Op::MethodCallSpread) {
                    named.emplace(inst.icIndex, Named{inst.op, inst.keyIndex});
                }
            }
        }
    }
    for (const il::Function& fn : probe.functions) {
        if (fn.isExternal || harvest.samples.empty()) continue;
        const auto it = harvest.samples.find(il::functionNameHash(fn.name));
        if (it == harvest.samples.end()) continue;
        plan.samples[fn.name] += it->second;
        plan.stats.harvestSamples += it->second;
    }
    constexpr uint64_t W = kHarvestExecutions;
    for (const auto& [ic, site] : harvest.ics) {
        const auto it = named.find(ic);
        if (it == named.end() || site.ways.empty()) continue;
        const il::Op op = it->second.op;
        const bool method = op == il::Op::MethodCall || op == il::Op::MethodCallSpread;
        if (method != site.method) continue;  // not what this index names here
        ++plan.stats.harvestSites;
        const bool uncached = site.uncached >= kHarvestUncachedMin;
        if (method) {
            ++plan.stats.harvestMethod;
            const bool more = site.mega || uncached;
            const uint64_t n = site.ways.size();
            const uint64_t share = more ? W / (2 * n) : W / n;
            NameCounts& shapes = plan.methodShapesByIc[ic];
            NameCounts& callees = plan.methodsByIc[ic];
            for (const HarvestWay& way : site.ways) {
                shapes.counts[way.receiver] += share;
                callees.counts[way.callee] += share;
            }
            if (more) {
                shapes.counts["*other*"] += W - share * n;
                callees.counts["*other*"] += W - share * n;
            }
            shapes.total += more ? W : share * n;
            callees.total += more ? W : share * n;
            continue;
        }
        ++plan.stats.harvestProperty;
        const bool write = op == il::Op::PropSet;
        const std::string& key = it->second.key < probe.keyConstants.size() ? probe.keyConstants[it->second.key]
                                                                               : std::string();
        // Receivers, each once; a read's receiver answered undefined when a
        // way held the key absent for it.
        std::map<std::string, bool> receivers;
        std::set<std::string> stored;
        for (const HarvestWay& way : site.ways) {
            bool& absent = receivers[way.receiver];
            absent = absent || way.form == "absent";
            if (way.form == "store") stored.insert(way.receiver);
        }
        const bool more = site.full || (write && site.mega) || uncached;
        const uint64_t n = receivers.size();
        if (site.ways.size() > n) plan.shapeVariantIcs.insert(ic);
        const uint64_t share = more ? W / (2 * n) : W / n;
        NameCounts& counts = plan.shapesByIc[ic];
        uint64_t total = 0;
        const std::string* grown = nullptr;
        const std::string* from = nullptr;
        if (write && n == 2) {
            const std::string& a = receivers.begin()->first;
            const std::string& b = std::next(receivers.begin())->first;
            if (grewFrom(a, b)) grown = &a, from = &b;
            if (grewFrom(b, a)) grown = &b, from = &a;
        }
        for (const auto& [receiver, absent] : receivers) {
            const uint64_t mine = &receiver == grown ? 2 * share - 1 : &receiver == from ? 1 : share;
            if (write && stored.count(receiver) == 0 && lastKeyIs(receiver, key)) {
                counts.counts[receiver] += mine / 2;
                counts.counts["*other*"] += mine - mine / 2;
            } else {
                counts.counts[receiver] += mine;
            }
            total += mine;
        }
        if (more) {
            counts.counts["*other*"] += W - total;
            total = W;
        }
        counts.total += total;
        if (!write) {
            ReadResults& results = plan.readResultsByIc[ic];
            results.total += total;
            for (const auto& [receiver, absent] : receivers) results.undefineds += absent ? share : 0;
        }
    }
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
    if (const Harvest* harvest = profile.harvest()) planHarvest(probe, *harvest, plan);
    // Every callee a method site names, not only the dominant ones the claims
    // spend: function placement (lower/function_placement.h) reads them all.
    std::vector<std::string> wanted;
    for (const auto& [ic, names] : plan.methodsByIc) {
        for (const auto& [name, count] : names.counts) {
            if (name != "*other*") wanted.push_back(name);
        }
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
    if (plan.stats.harvest) {
        out += plan.stats.harvestStale
                   ? std::string("feedback: harvest from another build (IC layout digest differs): ignored\n")
                   : "feedback: harvest sites " + n(plan.stats.harvestSites) + " (property " +
                         n(plan.stats.harvestProperty) + ", method " + n(plan.stats.harvestMethod) +
                         "), samples " + n(plan.stats.harvestSamples) + "; no value sites (C, D)\n";
    }
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
    out += "feedback: F accessor targets get " + n(stats.accessorGets) + ", set " + n(stats.accessorSets) +
           " (dominant shape lacking the key with no one accessor " + n(stats.accessorNone) +
           "); profiled executions " + n(stats.accessorExecutions) + "\n";
    return out;
}

}  // namespace bronze::spec
