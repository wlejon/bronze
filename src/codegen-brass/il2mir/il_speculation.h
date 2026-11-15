#pragma once

// Speculation sites (docs/il2mir.md, section 7).
//
// The mechanism is brass's (brass/mir/speculation_sites.hpp): an inline fast
// path that is correct only for some operands (an IC hit, a number/number
// operation) is lowered as `br_if %hit, fast, slow` behind a placeholder
// guard whose resume target is `slow`; the slow path counts its runs, and
// tier 2 arms the guards of the sites whose counts say they stay on the fast
// path. What is bronze's here: the kinds of site and the policy that reads
// their counts.
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
#include <string>
#include <string_view>
#include <vector>

namespace il2mir {

using brass::SpecFeedback;

enum class SpecKind : uint8_t {
    // A number/number arithmetic fast path: armed only if its slow path
    // never ran.
    Arith,
    // An inline-cache hit: armed while its misses stay within the cold
    // fills a monomorphic site takes.
    Property,
};

// A program's speculation feedback, armed by bronze's policy.
std::shared_ptr<SpecFeedback> make_spec_feedback();

// The name the sites give the program's entry function, whose symbol
// differs between a JIT run and an AOT build of the same program.
inline constexpr std::string_view kSpecEntryName = "<entry>";

// The per-lowering half: brass's site emitter.
class SpecSiteEmitter {
public:
    void set_feedback(SpecFeedback* feedback) { sites_.set_feedback(feedback); }
    [[nodiscard]] bool has_feedback() const noexcept { return sites_.has_feedback(); }
    // Starts a function; `can_deopt` false lowers every site as its branch.
    // `name` is its site name (kSpecEntryName for the entry).
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
    // IMAGE SITES. An image has no lower tier, so a guard there exits into
    // the function's resume twin, its generic code compiled beside it (brass
    // docs/speculation_and_deopt.md, section 7; make_resume_twins below).
    // With this set, a function begun with `can_deopt` lowers the sites its
    // lowering trusts through site_branch as guards; every other site, and
    // every site of any other function, stays a branch.
    void set_image_sites(bool on) { sites_.set_image_sites(on); }
    // `br_if %hit, fast, slow`, `slow` being the generic code of the
    // operation `hit` tests (it takes no parameters). With `exits`, in a
    // function that takes guards, the branch is an image site: the speculated
    // function leaves through its twin on a miss and `slow` is the twin's.
    // The builder stays in the block it ended.
    void site_branch(Builder& b, Value* hit, BasicBlock* fast, BasicBlock* slow, bool exits,
                     const std::vector<Value*>& fast_args = {});
    // Whether site_branch makes guards in the function being lowered.
    [[nodiscard]] bool image_sites_here() const noexcept { return sites_.image_sites() && sites_.can_deopt(); }
    // Whether the module is an image's, whose functions make_resume_twins
    // sees: where the fact markers below may be emitted.
    [[nodiscard]] bool image_sites() const noexcept { return sites_.image_sites(); }

private:
    brass::SpecSiteEmitter sites_;
};

// An image module lowered with image sites, before any pass: every function
// holding one gets its resume twin (named resume_twin_name), its guards armed
// and pointed at the twin. A function whose twin cannot be built keeps its
// branches.
//
// FAILURE POLICY. Each such function `f` also gets its generic entry
// `f__generic` (f as lowered, every site a branch), a probe `f__probe`, and
// an entry cell `__bronze_twin_entry_f` in the image's shared data (names
// and layout in il_abi.h, the cell defined in brass_backend_sections.cpp):
// the word every direct call of f in the module goes through (`call @f via
// @cell`, one instruction as before), then two counts. Nothing of it is in
// f. The cell's word is one of
//   f           trusted. The twin's guard entries count f's exits, and the
//               kTwinExitLimit'th starts a probe.
//   f__probe    counts the call and calls f. kTwinExitLimit exits before
//               kTwinProbeCalls calls: f fails too often, generic from now
//               on. Else f is trusted again, counts at zero.
//   f__generic  for good.
// So a function is given up when more than one call in
// kTwinProbeCalls / kTwinExitLimit leaves, and a function that leaves now
// and then pays a probe's count for kTwinProbeCalls calls each
// kTwinExitLimit exits. A site's identity cell is filled by the generic code
// its first exit runs, so every function leaves a few times as it warms up.
//
// SLOW ARMS (docs/type-feedback.md, "Slow paths"). An inline fast path's
// generic fallback, the slow arm of an instruction with a slow site (il.h
// `slowSite`), is reached through PropertyLoweringHelper::slow_branch and
// begins with slow_arm. In a build that may be harvested slow_arm bumps the
// site's counter and marks the site armed. Every slow arm of one instruction
// shares its counter.
//
// ARMS. An instruction with a slow site has kSlowSiteArms indices, the first
// its slow arm's and the rest its arms', which its lowering marks where each
// begins (PropertyLoweringHelper::arm_begin) in a build that may be
// harvested.
//
// PROVEN FACTS. A fact about a value that nothing can undo (that it is a
// plain object: its tag is the value, its heap kind set once at creation)
// holds wherever a block that established it dominates. Lowering cannot see
// that: an exit's miss leaves the speculated function and rejoins in the
// twin, and the two are cut from one lowering. So it marks instead, with two
// calls that exist only until make_resume_twins has cut every copy:
// `__bronze_fact_proof(id)` in a block only a value with the fact reaches,
// `i32 __bronze_fact_proven(id)` where a later test of that value would run
// (PropertyLoweringHelper::prove_plain / proven_plain). Each copy then
// resolves its own: a `proven` is 1 where a `proof` of its id dominates it
// in that copy's control flow, else 0, and the proofs go. The speculated
// function, whose misses left, keeps few tests; the generic copy keeps those
// behind a join with a miss; the twin, entered at its resume blocks, nearly
// all.
void make_resume_twins(Module& mod);
inline constexpr int64_t kTwinExitLimit = 256;
inline constexpr int64_t kTwinProbeCalls = 8 * kTwinExitLimit;

// brass's tier-2 front pass for a program lowered with `feedback`.
void apply_tier2_speculation(Module& mod, const SpecFeedback& feedback);

} // namespace il2mir
