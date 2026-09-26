#pragma once

// Speculation profiles: the JIT's site feedback carried to an AOT build
// (docs/il2mir.md, section 8).
//
// A program run under the tiered pipeline with BRONZE_SPEC_PROFILE_OUT=<path>
// writes every speculation site's miss count at exit. `bronze build
// --spec-profile <path>` reads it back: the AOT lowering then inlines every
// keyed property site the way the tiered lowering does, and each site's
// branch is laid out as its counts say (a slow path that stayed cold goes to
// the end of the function, a megamorphic read skips its poly scan). AOT has
// no lower tier to deoptimize to, so every slow path stays a branch; a
// profile only moves blocks, never removes a path.
//
// A site is named by its function (kSpecEntryName for the entry), its kind,
// its tag (the property key, the arithmetic helper, the builtin's key) and
// its occurrence among the function's sites of that kind and tag. The JIT
// and AOT lowerings of one program need not agree on every site (inference,
// the eval wrapper and the builtin fast paths differ between them); a site
// the profile does not name is lowered as without one.
//
// The format is line-oriented text, versioned by its first line:
//
//   bronze-spec-profile 1
//   program
//   s <kind> <misses> <function> <tag>
//
// `kind` is SpecKind's value; `function` and `tag` are percent-encoded
// (bytes outside 0x21..0x7e and '%' as %XX, the empty string as a lone %).
// Each `program` line starts one tiered program's sites, in the order it
// lowered them; a function a program earlier in the file already named is
// ignored in a later one (the entry of the first program is the script's).

#include "il_speculation.h"

#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace il2mir {

inline constexpr uint32_t kSpecProfileVersion = 1;

class SpecProfile {
public:
    // Null with `err` set when the file cannot be read or is not a profile
    // of this version.
    static std::shared_ptr<const SpecProfile> read_file(const std::string& path, std::string& err);
    static std::shared_ptr<const SpecProfile> parse(std::istream& in, std::string& err);

    // The misses recorded for occurrence `n` of (`kind`, `tag`) in `fn`.
    std::optional<uint32_t> misses(std::string_view fn, SpecKind kind, std::string_view tag, uint32_t n) const;
    size_t site_count() const noexcept { return site_count_; }

private:
    std::unordered_map<std::string, std::vector<uint32_t>> sites_;
    size_t site_count_ = 0;
};

// The key a site's (function, kind, tag) is counted under.
std::string spec_site_key(std::string_view fn, SpecKind kind, std::string_view tag);

// Writes the programs' sites as one profile.
void write_spec_profile(std::ostream& out, const std::vector<std::vector<SpecFeedback::SiteRecord>>& programs);

// With BRONZE_SPEC_PROFILE_OUT set, keeps `feedback` for the profile written
// to that path at process exit; otherwise nothing.
void register_spec_profile_dump(std::shared_ptr<SpecFeedback> feedback);

} // namespace il2mir
