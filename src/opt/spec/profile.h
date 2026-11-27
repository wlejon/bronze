#pragma once

// A TYPE-FEEDBACK HARVEST PROFILE read back (`bronze build --feedback-in <file.tfb>`):
// what an uninstrumented `--harvest-out` run's inline caches and slow-site
// counters observed.
//
// The parser is strict: an unknown version or malformed row is an error naming
// the line, not a skipped row.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "abi/bronze_feedback_site.h"
#include "il/feedback_site.h"

namespace bronze::spec {

// An INLINE-CACHE HARVEST (docs/type-feedback.md, "Harvest"; the runtime's
// ic_harvest.h writes it): what each IC site's cache held when an ordinary
// build's run was read back, by IC index. No counts: a cache holds shapes,
// not executions.
struct HarvestWay {
    // Property sites: `own`, `proto`, `absent`, `accessor`, `array`; method
    // sites: `direct`, `slot`, `native`.
    std::string form;
    std::string receiver;
    // Method sites: the callee, `*other*` when the entry does not say. A
    // property site's `accessor` way (version 7 on): the getter here and the
    // setter in `setter`, each `*other*` when the harvest could not name it.
    // A property site's `proto` way (version 8 on): the links from the
    // receiver to the holder of the data property here, its slot there in
    // `setter`, both decimal; empty when the harvest does not say.
    std::string callee;
    std::string setter;
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
    // The `callee` rows: a call's or a `new`'s last callee its code entered
    // directly, by the slow-site index of the word that held it (the same
    // kLastShapeArm word a named site keeps its shape in); `*other*` for one
    // with no source name (a native). The site's kShapeTurnArm count is how
    // many times its callee changed, the first included.
    std::map<uint32_t, std::string> callees;
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

    // The file's harvest section, or null when empty.
    const Harvest* harvest() const { return harvest_ ? &*harvest_ : nullptr; }
    void setHarvest(Harvest harvest) { harvest_ = std::move(harvest); }

private:
    std::optional<Harvest> harvest_;
};

}  // namespace bronze::spec
