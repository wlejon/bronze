#include "il_speculation.h"
#include "il_ic_stub.h"
#include "il_spec_profile.h"

#include "abi/bronze_abi.h"

#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>

#include <cstdlib>

namespace il2mir {

namespace {

// A monomorphic property site misses once per inline-cache fill: the first
// run, and a refill after the site's way 0 was displaced. More misses than
// this before tier-up says the site sees several shapes.
constexpr uint32_t kPropertyColdMisses = 2;
// A polymorphic site's helper runs once per way it fills, and again when a
// fill is displaced; twice the ways leaves room for that and still refuses a
// megamorphic site, whose helper runs on most reads.
constexpr uint32_t kPolyPropertyColdMisses = 2 * BRONZE_ABI_IC_WAYS;

// BRONZE_SPEC_ARM_KINDS=<mask>: the kinds armed (should_arm).
uint32_t arm_kind_mask() {
    static const uint32_t mask = [] {
        const char* v = std::getenv("BRONZE_SPEC_ARM_KINDS");
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : 0xEu;
    }();
    return mask;
}

bool should_arm(uint8_t raw_kind, uint32_t misses, bool ran) {
    // BRONZE_SPEC_ARM_KINDS=<mask>: arm only the kinds whose bit is set (1
    // arith, 2 property, 4 call target, 8 poly property), the per-kind A/B.
    // Arith sites are not armed by default. Arming them once cost nbody time
    // because brass's register allocator kept every guard state value live to
    // the guard's exit block at the end of the function (176 spill slots in
    // nbody's advance armed against 118 unarmed); brass now ends that
    // liveness at the guard's branch (45 against 88), and armed arith is
    // about 12 ms faster on nbody. What still keeps it off: a site whose
    // slow path never ran is armed whether or not it ran at all, so an OSR
    // copy of a top-level script arms the arithmetic after its loop, which
    // then deopts on its first string `+` (cpu_work's strings phase, 47 ms
    // to 70). An armed arith site now records that it ran (`ran`, a flag
    // the lower tiers set each time it is reached; make_spec_feedback), and
    // one that never ran keeps its branch.
    if ((arm_kind_mask() & (1u << raw_kind)) == 0) return false;
    // A property site's inline cache starts empty, so its first run misses:
    // one with no misses has never run, and says nothing about the shapes
    // it will see. Armed, its first run fails the guard, and the frame
    // finishes in Tier 0 until the code is compiled again; a top-level loop
    // OSR'd before the code after it ran lost its whole OSR entry that way.
    // Left a branch, its first run takes the slow path and carries on.
    switch (static_cast<SpecKind>(raw_kind)) {
        case SpecKind::Arith: return ran && misses == 0;
        case SpecKind::CallTarget: return misses == 0;
        case SpecKind::Property: return misses != 0 && misses <= kPropertyColdMisses;
        case SpecKind::PolyProperty: return misses != 0 && misses <= kPolyPropertyColdMisses;
    }
    return false;
}

} // namespace

std::shared_ptr<SpecFeedback> make_spec_feedback() {
    auto feedback = std::make_shared<SpecFeedback>(&should_arm);
    // The ran flag is a store on every run of the site in the lower tiers,
    // paid only where arith sites are armed at all.
    const uint32_t arith = 1u << static_cast<uint32_t>(SpecKind::Arith);
    if (arm_kind_mask() & arith) feedback->set_ran_kinds(arith);
    return feedback;
}

void SpecSiteEmitter::begin_function(bool can_deopt, std::string_view name) {
    fn_name_ = name;
    occurrences_.clear();
    // BRONZE_SPEC_NO_GUARDS=1: the sites and their counts, but no guards
    // (the lowering A/B: every function lowers as if it could not deopt).
    static const bool no_guards = [] {
        const char* v = std::getenv("BRONZE_SPEC_NO_GUARDS");
        return v && v[0] == '1';
    }();
    sites_.begin_function(can_deopt && !no_guards, name);
}

std::optional<uint32_t> SpecSiteEmitter::peek_profile(SpecKind kind, std::string_view tag) const {
    if (!profile_) return std::nullopt;
    auto it = occurrences_.find(spec_site_key({}, kind, tag));
    return profile_->misses(fn_name_, kind, tag, it == occurrences_.end() ? 0 : it->second);
}

void SpecSiteEmitter::skip_site(SpecKind kind, std::string_view tag) {
    if (profile_) ++occurrences_[spec_site_key({}, kind, tag)];
}

bool SpecSiteEmitter::profile_says_polymorphic(uint32_t misses) {
    return misses > kPropertyColdMisses;
}

bool SpecSiteEmitter::apply_profile(Builder& b, Value*& hit, BasicBlock* slow, SpecKind kind, std::string_view tag) {
    const std::string key = spec_site_key({}, kind, tag);
    const uint32_t n = occurrences_[key]++;
    const std::optional<uint32_t> misses = profile_->misses(fn_name_, kind, tag, n);
    if (!misses) return false;
    bool cold = false;
    switch (kind) {
        case SpecKind::Arith:
        case SpecKind::CallTarget: cold = *misses == 0; break;
        case SpecKind::Property: cold = *misses <= kPropertyColdMisses; break;
        case SpecKind::PolyProperty:
            cold = *misses <= kPolyPropertyColdMisses;
            // Megamorphic: the scan would miss as the helper does, so the
            // way-0 miss goes to the helper, as it did before the scan.
            if (!cold) hit = b.build_iconst_i32(0);
            break;
    }
    // brass's block layout sinks a block whose name says cold to the end of
    // the function (codegen/block_layout.cpp), off the fast path's line.
    if (cold) {
        Module* mod = slow->parent() ? slow->parent()->parent() : nullptr;
        if (mod) slow->set_name(mod->string_pool().intern(std::string(slow->name()) + "_cold"));
    }
    return true;
}

void SpecSiteEmitter::emit_branch(Builder& b, Value* hit, BasicBlock* fast, BasicBlock* slow, SpecKind kind,
                                  std::string_view tag, Value** counter_out) {
    if (profile_ && !sites_.has_feedback()) apply_profile(b, hit, slow, kind, tag);
    sites_.emit_branch(b, hit, fast, slow, static_cast<uint8_t>(kind), tag, counter_out);
}

void apply_tier2_speculation(Module& mod, const SpecFeedback& feedback) {
    // BRONZE_SPEC_TRACE=1: one line per function copy with guards.
    // BRONZE_SPEC_NO_ARM=1: arm nothing (the guards-vs-branches A/B).
    // BRONZE_SPEC_TRACE=2: also one line per armed site (its id is the
    // guard= a brass tier log names on a deopt).
    static const int trace = [] {
        const char* v = std::getenv("BRONZE_SPEC_TRACE");
        return v && (v[0] == '1' || v[0] == '2') ? v[0] - '0' : 0;
    }();
    static const bool no_arm = [] {
        const char* v = std::getenv("BRONZE_SPEC_NO_ARM");
        return v && v[0] == '1';
    }();
    brass::ArmSpeculationOptions options;
    options.arm_none = no_arm;
    options.trace = trace;
    // What survives of the reads' out-of-line scans goes back inline.
    options.after_function = [](Function& fn) { expand_ic_stubs(fn); };
    brass::arm_speculation_sites(mod, feedback, options);
}

} // namespace il2mir
