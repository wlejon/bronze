#pragma once

// Speculation sites (docs/il2mir.md, section 7).
//
// The mechanism is brass's (brass/mir/speculation_sites.hpp): an inline fast
// path that is correct only for some operands (an IC hit, a number/number
// operation) is lowered as `br_if %hit, fast, slow` behind a placeholder
// guard whose resume target is `slow`; the slow path counts its runs, and
// tier 2 arms the guards of the sites whose counts say they stay on the fast
// path. What is bronze's here: the kinds of site, the policy that reads their
// counts, and an AOT build's profile (il_spec_profile.h), which lays each
// site out as a recorded run's counts say instead of guarding it.
//
// Every function carries guards: nothing its slow paths read is wrong in the
// fresh Tier-0 frame a deopt resumes in. An argv block (an alloca) is fine:
// brass re-creates an alloca in a guard's state in the resuming frame, with
// its contents. So is a coroutine body: brass's coroutine lowering files
// suspend resume blocks under ids with the top bit set, apart from the
// guards' site numbers, and completes each guard's state with the frame and
// the values it reloads, so a deopt finishes that resume in Tier 0 over the
// same heap frame and the next resume dispatches as usual.

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <brass/mir/speculation_sites.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace il2mir {

using brass::SpecFeedback;

enum class SpecKind : uint8_t {
    // A number/number arithmetic fast path: armed only if its slow path
    // never ran.
    Arith,
    // An inline-cache hit: armed while its misses stay within the cold
    // fills a monomorphic site takes.
    Property,
    // A call whose target is the builtin its key names (a `Math` member)
    // with a Number argument, lowered to the machine op: armed only if the
    // generic call never ran (the target never was anything else).
    CallTarget,
    // A property read's scan of its site's other ways, entered on a way-0
    // miss: armed while the helper ran only for the fills a site of up to
    // BRONZE_ABI_IC_WAYS shapes takes (a megamorphic site keeps its branch).
    PolyProperty,
};

// A program's speculation feedback, armed by bronze's policy.
std::shared_ptr<SpecFeedback> make_spec_feedback();

class SpecProfile;

// The name a profile gives the program's entry function, whose symbol
// differs between a JIT run and an AOT build of the same program.
inline constexpr std::string_view kSpecEntryName = "<entry>";

// The per-lowering half: brass's site emitter, or, in an AOT lowering with a
// profile, the profile's layout of each site.
class SpecSiteEmitter {
public:
    void set_feedback(SpecFeedback* feedback) { sites_.set_feedback(feedback); }
    [[nodiscard]] bool has_feedback() const noexcept { return sites_.has_feedback(); }
    // An AOT lowering with a profile (il_spec_profile.h): each site's branch
    // stays a branch (no guard, no count), laid out as the profile's counts
    // say, and a site the profile shows megamorphic skips its poly scan.
    void set_profile(const SpecProfile* profile) { profile_ = profile; }
    [[nodiscard]] bool has_profile() const noexcept { return profile_ != nullptr; }
    // Starts a function; `can_deopt` false lowers every site as its branch.
    // `name` is its profile name (kSpecEntryName for the entry).
    void begin_function(bool can_deopt, std::string_view name = {});
    // brass::SpecSiteEmitter's replay of a body built again.
    void begin_replay(uint32_t first, uint32_t count) { sites_.begin_replay(first, count); }
    bool end_replay() { return sites_.end_replay(); }
    void begin_recording() { sites_.begin_recording(); }
    uint32_t sites_taken() const noexcept { return sites_.sites_taken(); }
    uint32_t first_site_taken() const noexcept { return sites_.first_site_taken(); }
    bool sites_consecutive() const noexcept { return sites_.sites_consecutive(); }
    // brass::SpecSiteEmitter::emit_branch for a site of `kind`.
    void emit_branch(Builder& b, Value* hit, BasicBlock* fast, BasicBlock* slow, SpecKind kind,
                     std::string_view tag = {}, Value** counter_out = nullptr);
    void finish_function(Function& fn) { sites_.finish_function(fn); }
    // With a profile: the recorded misses of the next site of (kind, tag),
    // without taking it; nullopt when the profile does not name it.
    std::optional<uint32_t> peek_profile(SpecKind kind, std::string_view tag) const;
    // With a profile: passes over the next site of (kind, tag) without
    // emitting it, so the ones after it keep their occurrence numbers.
    void skip_site(SpecKind kind, std::string_view tag);
    // Whether a Property site's recorded misses say it saw several shapes.
    static bool profile_says_polymorphic(uint32_t misses);

private:
    // The profile's decision for the next site of (kind, tag): `slow`
    // renamed cold, and a megamorphic scan's `hit` made false. False when
    // the profile does not name the site.
    bool apply_profile(Builder& b, Value*& hit, BasicBlock* slow, SpecKind kind, std::string_view tag);
    brass::SpecSiteEmitter sites_;
    const SpecProfile* profile_ = nullptr;
    std::string fn_name_;
    // Per (kind, tag): how many sites of the current function came before.
    std::unordered_map<std::string, uint32_t> occurrences_;
};

// brass's tier-2 front pass for a program lowered with `feedback`.
void apply_tier2_speculation(Module& mod, const SpecFeedback& feedback);

} // namespace il2mir
