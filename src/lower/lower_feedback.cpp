// THE TYPE-FEEDBACK INSTRUMENT, lowering half (lower_feedback.h).
//
// Every decision about WHICH sites exist is il/feedback_site.cpp's, because
// the build that reads a profile back has to enumerate the same sites without
// instrumenting anything. This file only turns that list into instructions and
// into the registration rows the runtime needs.

#include "lower/lower_feedback.h"

#include <unordered_map>
#include <vector>

#include "abi/bronze_feedback_site.h"
#include "il/feedback_site.h"

namespace bronze::lower {

namespace {

// The module's key pool as a lookup, interning what is missing. Feedback keys
// are new strings (a function key, the marker's name); a method name is
// already in the pool as its call's own key.
class KeyPool {
public:
    explicit KeyPool(il::Module& module) : module_(module) {
        for (size_t i = 0; i < module.keyConstants.size(); ++i) {
            index_.emplace(module.keyConstants[i], static_cast<uint32_t>(i));
        }
    }
    uint32_t intern(const std::string& key) {
        auto [it, inserted] = index_.emplace(key, static_cast<uint32_t>(module_.keyConstants.size()));
        if (inserted) module_.keyConstants.push_back(key);
        return it->second;
    }

private:
    il::Module& module_;
    std::unordered_map<std::string, uint32_t> index_;
};

il::Instruction makeRecord(const il::FeedbackSite& site, uint32_t fnKey,
                           const il::Instruction* anchor) {
    il::Instruction rec;
    rec.op = il::Op::FeedbackRecord;
    rec.type = il::Type::Void;
    rec.result = il::kNoValue;
    if (site.value != il::kNoValue) rec.operands = {site.value};
    rec.keyIndex = fnKey;
    rec.immI32 = static_cast<int32_t>(
        BRONZE_FEEDBACK_INFO(static_cast<uint32_t>(site.kind), site.ordinal));
    // The anchor's position, so a record attributes to the line and the
    // inlined frame of the instruction it observes.
    if (anchor != nullptr) {
        rec.span = anchor->span;
        rec.inlineFrame = anchor->inlineFrame;
    }
    return rec;
}

}  // namespace

bool instrumentFeedback(il::Module& module, const std::string& outPath, std::string* error,
                        FeedbackStats* stats) {
    if (!module.censusSites.empty()) {
        if (error != nullptr) {
            *error = "--feedback-out cannot instrument a module that also carries a pin census "
                     "(--census): the two instruments share one runtime channel";
        }
        return false;
    }
    // Keys first, over the IL as every pass left it: a function's hash is of
    // its uninstrumented body, which is what a reading build will hash.
    const std::vector<std::string> keys = il::feedbackFunctionKeys(module);
    KeyPool pool(module);

    std::vector<il::Module::CensusSiteEntry> rows;
    il::Module::CensusSiteEntry marker;
    marker.keyIndex = pool.intern("bronze-type-feedback");
    marker.info = BRONZE_FEEDBACK_INFO(BRONZE_FEEDBACK_KIND_MARKER, 0u);
    rows.push_back(marker);

    FeedbackStats local;
    for (size_t f = 0; f < module.functions.size(); ++f) {
        il::Function& fn = module.functions[f];
        const std::vector<il::FeedbackSite> sites = il::enumerateFeedbackSites(module, fn);
        if (sites.empty()) continue;
        const uint32_t fnKey = pool.intern(keys[f]);
        ++local.functions;
        local.sites += static_cast<uint32_t>(sites.size());

        // Per block, the records to put before and after each instruction.
        struct Slot {
            std::vector<const il::FeedbackSite*> before;
            std::vector<const il::FeedbackSite*> after;
        };
        std::vector<std::vector<Slot>> plan(fn.blocks.size());
        for (size_t b = 0; b < fn.blocks.size(); ++b) plan[b].resize(fn.blocks[b].instructions.size());
        std::vector<const il::FeedbackSite*> entry;
        for (const il::FeedbackSite& site : sites) {
            if (site.block == il::FeedbackSite::kEntrySite) {
                entry.push_back(&site);
            } else if (site.after()) {
                plan[site.block][site.inst].after.push_back(&site);
            } else {
                plan[site.block][site.inst].before.push_back(&site);
            }
            if (site.kind == il::FeedbackKind::Method) {
                // The method name travels as a registration row beside the
                // site, because a record carries one key and that is the
                // function's.
                il::Module::CensusSiteEntry a;
                a.keyIndex = fnKey;
                a.info = BRONZE_FEEDBACK_INFO(BRONZE_FEEDBACK_KIND_METHOD, site.ordinal);
                il::Module::CensusSiteEntry n;
                n.keyIndex = site.keyIndex;
                n.info = BRONZE_FEEDBACK_INFO(BRONZE_FEEDBACK_KIND_METHOD_NAME, 0u);
                rows.push_back(a);
                rows.push_back(n);
            }
        }

        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            std::vector<il::Instruction>& old = fn.blocks[b].instructions;
            std::vector<il::Instruction> out;
            out.reserve(old.size() + (b == 0 ? entry.size() : 0));
            if (b == 0) {
                const il::Instruction* anchor = old.empty() ? nullptr : &old.front();
                for (const il::FeedbackSite* s : entry) out.push_back(makeRecord(*s, fnKey, anchor));
            }
            for (size_t i = 0; i < old.size(); ++i) {
                for (const il::FeedbackSite* s : plan[b][i].before) out.push_back(makeRecord(*s, fnKey, &old[i]));
                out.push_back(old[i]);
                for (const il::FeedbackSite* s : plan[b][i].after) out.push_back(makeRecord(*s, fnKey, &old[i]));
            }
            old = std::move(out);
        }
    }

    module.censusOutPath = outPath;
    module.censusSites = std::move(rows);
    if (stats != nullptr) *stats = local;
    return true;
}

}  // namespace bronze::lower
