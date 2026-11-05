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
// The parser is strict, like every reader in bronze: an unknown version, a
// malformed line or a row outside a function is an error naming the line, not
// a skipped row.

#include <array>
#include <cstdint>
#include <map>
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

private:
    std::map<std::string, FunctionFeedback> functions_;
};

}  // namespace bronze::spec
