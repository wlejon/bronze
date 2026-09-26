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
// A function carries guards only when nothing its slow paths read would be
// wrong in the fresh Tier-0 frame a deopt resumes in: no coroutine body, no
// argv block (a pointer into the tier-2 frame's stack).

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace il2mir {

enum class SpecKind : uint8_t {
    // A number/number arithmetic fast path: armed only if its slow path
    // never ran.
    Arith,
    // An inline-cache hit: armed while its misses stay within the cold
    // fills a monomorphic site takes.
    Property,
};

// One program's speculation sites: per site, its kind and how many times its
// slow path ran. The counters live as long as the object, at stable
// addresses the lowered code increments directly.
class SpecFeedback {
public:
    uint32_t add_site(SpecKind kind);
    uint32_t* counter(uint32_t site);
    // Whether tier 2 should arm site `site`'s guard.
    bool should_speculate(uint32_t site) const;
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::deque<uint32_t> misses_;
    std::deque<SpecKind> kinds_;
};

// The per-lowering half: emits sites and fills their guards' state maps.
class SpecSiteEmitter {
public:
    void set_feedback(SpecFeedback* feedback) { feedback_ = feedback; }
    // Starts a function; `can_deopt` false lowers every site as its branch.
    void begin_function(bool can_deopt);
    // Ends the current block with the site's branch to `fast` / `slow` (and
    // its guard, where the function can deopt), then leaves the builder at
    // the end of `slow`, after its miss count. `slow` must be new and empty.
    void emit_branch(Builder& b, Value* hit, BasicBlock* fast, BasicBlock* slow, SpecKind kind);
    // Gives each guard of the function its state: the values live into its
    // resume block. Run once the function's CFG is final.
    void finish_function(Function& fn);

private:
    struct Pending {
        Instruction* guard;
        BasicBlock* slow;
    };
    SpecFeedback* feedback_ = nullptr;
    bool can_deopt_ = false;
    std::vector<Pending> pending_;
};

// brass's tier-2 front pass for a program lowered with `feedback`.
void apply_tier2_speculation(Module& mod, const SpecFeedback& feedback);

} // namespace il2mir
