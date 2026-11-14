#pragma once

// A TYPE-FEEDBACK PROFILE read back (`bronze build --feedback-in <file.tfb>`,
// docs/type-feedback.md): what a run of a `--feedback-out` build saw at each
// site, queryable by the site's stable key.
//
// A site's key is (function key, ordinal), both from il/feedback_site.h:
// `feedbackFunctionKeys(module)[i]` names function i of the module being
// compiled, and an ordinal is the site's position in
// `enumerateFeedbackSites(module, fn)`. A reading build that has not changed a
// function finds its sites under the same keys; one that has finds nothing,
// which is the answer it should get — feedback about a different body is not
// feedback about this one.
//
// The same file can carry an inline-cache HARVEST instead (or as well): what
// an uninstrumented build's caches held, by IC index (`Harvest` below).
//
// The parser is strict, like every reader in bronze: an unknown version, a
// malformed line or a row outside a function is an error naming the line, not
// a skipped row. Versions 1 and 2 are read; 2 lists every key of a shape
// (1 cut the list at 32 with `,+N`) and may carry a harvest.

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "abi/bronze_feedback_site.h"
#include "il/feedback_site.h"

namespace bronze::spec {

using TagCounts = std::array<uint64_t, BRONZE_FEEDBACK_TAG_COUNT>;

// The tag spelling the file uses (`f64`, `i32`, ...), and back. -1 for a name
// that is not a tag.
const char* tagName(uint32_t tag);
int tagFromName(std::string_view name);

struct SiteFeedback {
    uint32_t ordinal = 0;
    il::FeedbackKind kind = il::FeedbackKind::Value;
    uint64_t total = 0;
    TagCounts tags{};
    // Largest first; `*other*` (if present) is the folded tail.
    std::vector<std::pair<std::string, uint64_t>> shapes;
    std::vector<std::pair<std::string, uint64_t>> targets;

    // Bit t set when tag t was seen.
    uint32_t tagMask() const;
    // Exactly one tag seen (and the site was reached).
    bool monomorphic() const;
    // The one shape / callee every observation had, or empty.
    std::string soleShape() const;
    std::string soleTarget() const;
};

struct FunctionFeedback {
    std::string key;
    uint64_t entries = 0;
    std::map<uint32_t, SiteFeedback> sites;
};

// An INLINE-CACHE HARVEST (docs/type-feedback.md, "Harvest"; the runtime's
// ic_harvest.h writes it): what each IC site's cache held when an ordinary
// build's run was read back, by IC index. No counts: a cache holds shapes,
// not executions; feedback_plan.cpp says how a site maps onto the thresholds.
struct HarvestWay {
    // Property sites: `own`, `proto`, `absent`, `accessor`, `array`; method
    // sites: `direct`, `slot`, `native`.
    std::string form;
    std::string receiver;
    // Method sites only: the callee, `*other*` when the entry does not say.
    std::string callee;
};

struct HarvestSite {
    bool method = false;
    bool full = false;  // a read whose four ways were in use
    bool mega = false;  // the stub's table answered a write or a method call
    // The `other` row: the times the site reached the runtime with a receiver
    // no way could hold (an Array, a primitive, a dictionary, ...), which the
    // ways therefore do not show (ic_harvest.h rtIcHarvestNoteUncached).
    uint64_t uncached = 0;
    std::vector<HarvestWay> ways;
};

struct Harvest {
    uint64_t digest = 0;  // il/feedback_site.h icLayoutDigest of the build harvested
    uint32_t sites = 0;
    std::map<uint32_t, HarvestSite> ics;
    // The slow sites (il.h `slowSite`; ic_harvest.h): `slowSites` is the
    // harvested build's count (0: a harvest that did not count them),
    // `slowArmed` the sites it lowered with a counting fallback (bit i of
    // word i / 64), and `slow` the count of each site whose generic code ran.
    uint32_t slowSites = 0;
    std::vector<uint64_t> slowArmed;
    std::map<uint32_t, uint64_t> slow;
    // The `last` rows: the shape of a named site's last receiver, by the
    // slow-site index of the word that held it (il/feedback_site.h
    // kLastShapeArm); `*other*` for one that was no plain object.
    std::map<uint32_t, std::string> last;
    // The run's samples (runtime/harvest_sampler.h): per function, by the
    // FNV-1a hash of its IL name (il/feedback_site.h functionNameHash), how
    // many of the sampler's interrupts its code took.
    std::map<uint32_t, uint64_t> samples;
    // The slot representation the run kept (runtime/ic_harvest.h,
    // rtIcHarvestNoteBirth): by receiver name, the positions in its key list
    // born a Number and found holding one on every shape of that name the
    // caches met, and every key something else was found under. What a
    // build reads off these is which keys to make eligible and which layout
    // cell members to require as doubles; the runtime holds both to the
    // truth, so a wrong harvest costs time.
    std::map<std::string, std::vector<uint32_t>> doubles;
    std::set<std::string> mixed;
    // The same for plain objects (slot_repr.h, the OBJECT representation):
    // by receiver name the positions born and kept holding a plain object,
    // and every key anything else was found under. What a build reads off
    // them is which keys to make eligible for an object slot and which layout
    // cell members and class family fields to require as ones.
    std::map<std::string, std::vector<uint32_t>> objects;
    std::set<std::string> nonObject;
};

class TypeProfile {
public:
    // Parses the file format; on failure returns nullopt and sets `error`
    // (with the 1-based line).
    static std::optional<TypeProfile> parse(std::string_view text, std::string* error);
    static std::optional<TypeProfile> load(const std::string& path, std::string* error);

    // The file format, byte-identical to what the runtime writes for the
    // same counts: the round trip is how the two writers are held together.
    std::string serialize() const;

    const FunctionFeedback* function(const std::string& fnKey) const;
    const SiteFeedback* site(const std::string& fnKey, uint32_t ordinal) const;
    // How many times the function was entered on the profiled run; 0 for one
    // the profile does not know (never run, changed, or no entry site).
    uint64_t entryCount(const std::string& fnKey) const;
    bool isHot(const std::string& fnKey, uint64_t minEntries) const;

    const std::map<std::string, FunctionFeedback>& functions() const { return functions_; }
    // Adds or replaces one function; for tests and for merging profiles.
    void put(FunctionFeedback fn);

    // The file's harvest section, or null for a recorded profile.
    const Harvest* harvest() const { return harvest_ ? &*harvest_ : nullptr; }
    void setHarvest(Harvest harvest) { harvest_ = std::move(harvest); }

private:
    std::map<std::string, FunctionFeedback> functions_;
    std::optional<Harvest> harvest_;
};

}  // namespace bronze::spec
