#include "il_speculation.h"
#include "il_spec_profile.h"

#include "abi/bronze_abi.h"

#include <brass/mir/block_liveness.hpp>
#include <brass/mir/cfg_simplify.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>

#include <algorithm>
#include <cstdio>
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

} // namespace

uint32_t SpecFeedback::add_site(SpecKind kind, std::string_view fn, std::string_view tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    misses_.push_back(0);
    kinds_.push_back(kind);
    fns_.emplace_back(fn);
    tags_.emplace_back(tag);
    return static_cast<uint32_t>(misses_.size() - 1);
}

std::vector<SpecFeedback::SiteRecord> SpecFeedback::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SiteRecord> out;
    out.reserve(misses_.size());
    for (size_t i = 0; i < misses_.size(); ++i) {
        const uint32_t misses = *static_cast<const volatile uint32_t*>(&misses_[i]);
        out.push_back({kinds_[i], misses, fns_[i], tags_[i]});
    }
    return out;
}

uint32_t* SpecFeedback::counter(uint32_t site) {
    std::lock_guard<std::mutex> lock(mutex_);
    return site < misses_.size() ? &misses_[site] : nullptr;
}

bool SpecFeedback::should_speculate(uint32_t site) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (site >= misses_.size()) return false;
    // Written by lowered code on any thread without a lock: a stale count
    // only makes the guess older, and a wrong guess costs a deopt.
    const uint32_t misses = *static_cast<const volatile uint32_t*>(&misses_[site]);
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
    // to 70). Arming it needs a "ran" signal the fast path does not give.
    static const uint32_t kind_mask = [] {
        const char* v = std::getenv("BRONZE_SPEC_ARM_KINDS");
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : 0xEu;
    }();
    if ((kind_mask & (1u << static_cast<uint32_t>(kinds_[site]))) == 0) return false;
    // A property site's inline cache starts empty, so its first run misses:
    // one with no misses has never run, and says nothing about the shapes
    // it will see. Armed, its first run fails the guard, and the frame
    // finishes in Tier 0 until the code is compiled again; a top-level loop
    // OSR'd before the code after it ran lost its whole OSR entry that way.
    // Left a branch, its first run takes the slow path and carries on.
    switch (kinds_[site]) {
        case SpecKind::Arith:
        case SpecKind::CallTarget: return misses == 0;
        case SpecKind::Property: return misses != 0 && misses <= kPropertyColdMisses;
        case SpecKind::PolyProperty: return misses != 0 && misses <= kPolyPropertyColdMisses;
    }
    return false;
}

size_t SpecFeedback::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return misses_.size();
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
    can_deopt_ = can_deopt && feedback_ != nullptr && !no_guards;
    pending_.clear();
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
                                  std::string_view tag) {
    uint32_t* count = nullptr;
    if (profile_ && !feedback_) apply_profile(b, hit, slow, kind, tag);
    if (feedback_) {
        const uint32_t site = feedback_->add_site(kind, fn_name_, tag);
        count = feedback_->counter(site);
        if (can_deopt_) {
            // The label names no function: the guard has no exit stub, only
            // its resume target.
            Instruction* guard = b.build_guard(b.build_iconst_i32(1), "bronze.spec");
            guard->set_resume_id(site);
            b.current_block()->parent()->add_resume_point(site, slow);
            pending_.push_back({guard, slow});
        }
    }
    b.build_br_if(hit, fast, slow);
    b.position_at_end(slow);
    if (count) {
        Value* addr = b.build_iconst_i64(static_cast<int64_t>(reinterpret_cast<uintptr_t>(count)));
        Value* old = b.build_load(Type::i32(), addr, 0);
        b.build_store(Type::i32(), addr, 0, b.build_add(old, b.build_iconst_i32(1)));
    }
}

void SpecSiteEmitter::finish_function(Function& fn) {
    if (pending_.empty()) return;
    std::vector<const BasicBlock*> targets;
    targets.reserve(pending_.size());
    for (const Pending& p : pending_) targets.push_back(p.slow);
    const auto live_ins = block_live_ins(fn, targets);
    for (const Pending& p : pending_) {
        auto it = live_ins.find(p.slow);
        if (it == live_ins.end()) continue;
        std::vector<Value*> state;
        state.reserve(it->second.size());
        for (const Value* v : it->second) state.push_back(const_cast<Value*>(v));
        std::sort(state.begin(), state.end(), [](const Value* a, const Value* c) { return a->id() < c->id(); });
        p.guard->state_map() = std::move(state);
    }
    pending_.clear();
}

void apply_tier2_speculation(Module& mod, const SpecFeedback& feedback) {
    // BRONZE_SPEC_TRACE=1: one line per function copy with guards.
    // BRONZE_SPEC_NO_ARM=1: arm nothing (the guards-vs-branches A/B).
    static const bool trace = [] {
        const char* v = std::getenv("BRONZE_SPEC_TRACE");
        return v && v[0] == '1';
    }();
    static const bool no_arm = [] {
        const char* v = std::getenv("BRONZE_SPEC_NO_ARM");
        return v && v[0] == '1';
    }();
    const size_t sites = feedback.size();
    for (Function* fn : mod.functions()) {
        if (!fn || fn->block_count() == 0) continue;
        std::vector<uint32_t> seen;
        uint32_t armed = 0;
        for (BasicBlock* bb : fn->blocks()) {
            if (!bb) continue;
            Instruction* guard = nullptr;
            for (Instruction* inst : *bb) {
                if (inst && inst->opcode() == Opcode::guard) guard = inst;
            }
            if (!guard || guard->resume_id() >= sites) continue;
            Instruction* br = guard->next();
            Value* placeholder = guard->operand(0);
            const Instruction* def = placeholder ? placeholder->defining_instruction() : nullptr;
            if (!br || br->opcode() != Opcode::br_if || !def || def->opcode() != Opcode::iconst_i32 ||
                def->imm_i32() != 1) {
                continue;
            }
            const uint32_t site = guard->resume_id();
            seen.push_back(site);
            if (!no_arm && feedback.should_speculate(site)) {
                // The guard takes the hit test; the branch always goes fast.
                guard->set_operand(0, br->operand(0));
                br->set_operand(0, placeholder);
                ++armed;
            } else {
                bb->remove_instruction(guard);
            }
        }
        for (uint32_t site : seen) fn->remove_resume_point(site);
        if (armed != 0) {
            // A pinned branch leaves its slow side (a read's poly scan and
            // helper call) unreachable. Dropped here, before the tier-2
            // passes, so inlining, GVN and GVN-PRE never walk it.
            CfgSimplifyOptions prune;
            prune.enable_block_merge = false;
            prune.enable_param_elimination = false;
            prune.enable_trampoline_elimination = false;
            prune.max_iterations = 2;
            cfg_simplify_function(*fn, prune);
        }
        if (trace && !seen.empty()) {
            std::fprintf(stderr, "spec: %.*s armed %u of %zu\n", static_cast<int>(fn->name().size()),
                         fn->name().data(), armed, seen.size());
        }
    }
}

} // namespace il2mir
