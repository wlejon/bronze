#include "il_speculation.h"

#include <brass/mir/block_liveness.hpp>
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

} // namespace

uint32_t SpecFeedback::add_site(SpecKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    misses_.push_back(0);
    kinds_.push_back(kind);
    return static_cast<uint32_t>(misses_.size() - 1);
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
    // arith, 2 property, 4 call target), the per-kind A/B. Arith sites are
    // not armed by default: measured on nbody, arming them cost about 65 ms
    // of 420 against keeping the branch (the likely cause is that an armed
    // guard's state keeps the boxed operands, which the optimized fast path
    // otherwise never computes, live and computed; not yet confirmed).
    static const uint32_t kind_mask = [] {
        const char* v = std::getenv("BRONZE_SPEC_ARM_KINDS");
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : 0x6u;
    }();
    if ((kind_mask & (1u << static_cast<uint32_t>(kinds_[site]))) == 0) return false;
    switch (kinds_[site]) {
        case SpecKind::Arith:
        case SpecKind::CallTarget: return misses == 0;
        case SpecKind::Property: return misses <= kPropertyColdMisses;
    }
    return false;
}

size_t SpecFeedback::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return misses_.size();
}

void SpecSiteEmitter::begin_function(bool can_deopt) {
    // BRONZE_SPEC_NO_GUARDS=1: the sites and their counts, but no guards
    // (the lowering A/B: every function lowers as if it could not deopt).
    static const bool no_guards = [] {
        const char* v = std::getenv("BRONZE_SPEC_NO_GUARDS");
        return v && v[0] == '1';
    }();
    can_deopt_ = can_deopt && feedback_ != nullptr && !no_guards;
    pending_.clear();
}

void SpecSiteEmitter::emit_branch(Builder& b, Value* hit, BasicBlock* fast, BasicBlock* slow, SpecKind kind) {
    uint32_t* count = nullptr;
    if (feedback_) {
        const uint32_t site = feedback_->add_site(kind);
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
    const auto live_ins = block_live_ins(fn);
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
        if (trace && !seen.empty()) {
            std::fprintf(stderr, "spec: %.*s armed %u of %zu\n", static_cast<int>(fn->name().size()),
                         fn->name().data(), armed, seen.size());
        }
    }
}

} // namespace il2mir
