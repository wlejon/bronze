#pragma once

// Speculation sites (docs/il2mir.md, section 7).
//
// An inline fast path that is correct only for some operands (an IC hit, a
// number/number operation) is lowered as `br_if %hit, fast, slow`, where
// `slow` is the generic helper call. In a function that can deoptimize, the
// site also carries a GUARD in front of that branch: its condition is the
// constant 1 in the Tier-0 function, its state map every value live into
// `slow`, and `slow` is its resume target. Tier 0 and Tier 1 never fail it,
// so they run the branch; the guard is only there so that tier-2 code has a
// Tier-0 guard of the same resume id to finish the call at.
//
// The slow path counts its runs in the program's SpecFeedback. When a
// function tiers up, apply_tier2_speculation (brass's tier-2 front pass)
// reads the counts: a site whose slow path stayed cold gets the guard armed
// with `%hit` and its branch pinned to `fast`, so the tier-2 code is the
// fast path alone and a miss deoptimizes to Tier 0; any other site drops the
// guard and keeps the branch. The copy drops every resume point either way
// (the lower tier holds the resume blocks), which leaves the slow blocks of
// armed sites unreachable.
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
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace il2mir {

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

// One program's speculation sites: per site, its kind and how many times its
// slow path ran. The counters live as long as the object, at stable
// addresses the lowered code increments directly.
class SpecFeedback {
public:
    // `fn` is the site's function (kSpecEntryName for the program's entry)
    // and `tag` what the site tests (a property key, a helper): together
    // with the kind and the site's occurrence they name it in a profile
    // (il_spec_profile.h).
    uint32_t add_site(SpecKind kind, std::string_view fn = {}, std::string_view tag = {});
    uint32_t* counter(uint32_t site);
    // Whether tier 2 should arm site `site`'s guard.
    bool should_speculate(uint32_t site) const;
    // Site `site`'s guard failed often enough that tier 2 dropped the code
    // it was armed in: it is never armed again.
    void mark_failed(uint32_t site);
    size_t size() const;

    // One site as a profile records it.
    struct SiteRecord {
        SpecKind kind;
        uint32_t misses;
        std::string fn;
        std::string tag;
    };
    // Every site, in lowering order, with its count now.
    std::vector<SiteRecord> snapshot() const;
    // One site, with its count now.
    SiteRecord site(uint32_t site) const;

private:
    mutable std::mutex mutex_;
    std::deque<uint32_t> misses_;
    std::deque<uint8_t> failed_;
    std::deque<SpecKind> kinds_;
    std::deque<std::string> fns_;
    std::deque<std::string> tags_;
};

class SpecProfile;

// The name a profile gives the program's entry function, whose symbol
// differs between a JIT run and an AOT build of the same program.
inline constexpr std::string_view kSpecEntryName = "<entry>";

// The per-lowering half: emits sites and fills their guards' state maps.
class SpecSiteEmitter {
public:
    void set_feedback(SpecFeedback* feedback) { feedback_ = feedback; }
    // An AOT lowering with a profile (il_spec_profile.h): each site's branch
    // stays a branch (no guard, no count), laid out as the profile's counts
    // say, and a site the profile shows megamorphic skips its poly scan.
    void set_profile(const SpecProfile* profile) { profile_ = profile; }
    [[nodiscard]] bool has_profile() const noexcept { return profile_ != nullptr; }
    // Starts a function; `can_deopt` false lowers every site as its branch.
    // `name` is its profile name (kSpecEntryName for the entry).
    void begin_function(bool can_deopt, std::string_view name = {});
    // Ends the current block with the site's branch to `fast` / `slow` (and
    // its guard, where the function can deopt), then leaves the builder at
    // the end of `slow`, after its miss count. `slow` must be new and empty.
    void emit_branch(Builder& b, Value* hit, BasicBlock* fast, BasicBlock* slow, SpecKind kind,
                     std::string_view tag = {});
    // Gives each guard of the function its state: the values live into its
    // resume block. Run once the function's CFG is final.
    void finish_function(Function& fn);
    // With a profile: the recorded misses of the next site of (kind, tag),
    // without taking it; nullopt when the profile does not name it.
    std::optional<uint32_t> peek_profile(SpecKind kind, std::string_view tag) const;
    // With a profile: passes over the next site of (kind, tag) without
    // emitting it, so the ones after it keep their occurrence numbers.
    void skip_site(SpecKind kind, std::string_view tag);
    // Whether a Property site's recorded misses say it saw several shapes.
    static bool profile_says_polymorphic(uint32_t misses);

private:
    struct Pending {
        Instruction* guard;
        BasicBlock* slow;
    };
    // The profile's decision for the next site of (kind, tag): `slow`
    // renamed cold, and a megamorphic scan's `hit` made false. False when
    // the profile does not name the site.
    bool apply_profile(Builder& b, Value*& hit, BasicBlock* slow, SpecKind kind, std::string_view tag);
    SpecFeedback* feedback_ = nullptr;
    const SpecProfile* profile_ = nullptr;
    bool can_deopt_ = false;
    std::string fn_name_;
    // Per (kind, tag): how many sites of the current function came before.
    std::unordered_map<std::string, uint32_t> occurrences_;
    std::vector<Pending> pending_;
};

// brass's tier-2 front pass for a program lowered with `feedback`.
void apply_tier2_speculation(Module& mod, const SpecFeedback& feedback);

} // namespace il2mir
