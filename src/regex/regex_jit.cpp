#include "regex/regex_jit.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

#include "regex/regex_jit_dfa.h"
#include <brass/runtime/compile_pool.hpp>

namespace bronze::regex {

namespace {

// Loads a 16-bit UTF-16 code unit `units[unit_index]` as `i32` in `[0, 65535]`
// using `GrammarBuilder::load_byte` (little-endian low + high bytes).
brass::Value* emitLoadUtf16Unit(
    brass::codegen::GrammarBuilder& gb,
    brass::Value* units_ptr,
    brass::Value* unit_index) {
    using namespace brass;
    Builder& b = gb.builder();
    Value* byte_off = b.build_shl(unit_index, gb.const_i64(1));
    Value* lo_byte = gb.load_byte(units_ptr, byte_off);
    Value* hi_off = b.build_add(byte_off, gb.const_i64(1));
    Value* hi_byte = gb.load_byte(units_ptr, hi_off);
    Value* hi_shifted = b.build_shl(hi_byte, gb.const_i32(8));
    Value* u16_val = b.build_or(lo_byte, hi_shifted);
    // Redundant mask `(lo | (hi << 8)) & 0xFFFF` that `run_mir_superopt` proves redundant and elides.
    return b.build_and(u16_val, gb.const_i32(0xFFFF));
}

// Emits `regex_dfa_utf16_step` into `mod` using `brass::codegen::GrammarBuilder`.
// Combines UTF-16 byte reconstruction `(lo & 0xFF) | ((hi & 0xFF) << 8)`, redundant
// `& 0xFFFF` mask elision, non-ASCII guard, state bounds guard, and `dfa_table_step`.
void emitDfaUtf16StepFunction(brass::codegen::GrammarBuilder& gb, std::string_view name) {
    using namespace brass;
    Builder& b = gb.builder();
    Module* mod = b.current_module();
    Function* fn = mod->create_function(
        name, Type::i32(), {Type::ptr(), Type::i32(), Type::i32(), Type::i32(), Type::i32()});
    b.set_function(fn);

    BasicBlock* entry = b.append_block("entry");
    b.position_at_end(entry);
    Value* trans_table = b.add_block_param(entry, Type::ptr());
    Value* num_states = b.add_block_param(entry, Type::i32());
    Value* cur_state = b.add_block_param(entry, Type::i32());
    Value* lo_in = b.add_block_param(entry, Type::i32());
    Value* hi_in = b.add_block_param(entry, Type::i32());

    BasicBlock* valid_bb = b.create_block("valid_step");
    BasicBlock* invalid_bb = b.create_block("invalid_step");

    Value* lo = b.build_and(lo_in, gb.const_i32(0xFF));
    Value* hi = b.build_and(hi_in, gb.const_i32(0xFF));
    Value* u16_val = b.build_or(lo, b.build_shl(hi, gb.const_i32(8)));
    Value* u32_masked = b.build_and(u16_val, gb.const_i32(0xFFFF));
    Value* is_non_ascii = b.build_uge(u32_masked, gb.const_i32(128));
    Value* is_oob = b.build_uge(cur_state, num_states);
    Value* bail = b.build_or(is_non_ascii, is_oob);
    b.build_br_if(bail, invalid_bb, valid_bb);

    fn->append_block(invalid_bb);
    b.position_at_end(invalid_bb);
    b.build_ret(gb.const_i32(-1));

    fn->append_block(valid_bb);
    b.position_at_end(valid_bb);
    Value* ascii_byte = b.build_and(u32_masked, gb.const_i32(0x7F));
    Value* next_state = gb.dfa_table_step(trans_table, cur_state, ascii_byte);
    b.build_ret(next_state);
}

// Emits `regex_dfa_match_at` into `mod` using `brass::codegen::GrammarBuilder`.
void emitDfaMatchAtFunction(brass::codegen::GrammarBuilder& gb, std::string_view name) {
    using namespace brass;
    Builder& b = gb.builder();
    Module* mod = b.current_module();
    Function* fn = mod->create_function(
        name, Type::i64(),
        {Type::ptr(), Type::ptr(), Type::i32(), Type::ptr(), Type::i64(), Type::i64(), Type::ptr()});
    b.set_function(fn);

    BasicBlock* entry = b.append_block("entry");
    b.position_at_end(entry);

    Value* trans_table = b.add_block_param(entry, Type::ptr());
    Value* accept_table = b.add_block_param(entry, Type::ptr());
    Value* start_state = b.add_block_param(entry, Type::i32());
    Value* units_ptr = b.add_block_param(entry, Type::ptr());
    Value* length = b.add_block_param(entry, Type::i64());
    Value* start_pos = b.add_block_param(entry, Type::i64());
    Value* out_end_ptr = b.add_block_param(entry, Type::ptr());

    BasicBlock* valid_start_bb = b.create_block("valid_start");
    BasicBlock* early_no_match_bb = b.create_block("early_no_match");
    BasicBlock* loop_head_bb = b.create_block("loop_head");
    BasicBlock* check_char_bb = b.create_block("check_char");
    BasicBlock* step_dfa_bb = b.create_block("step_dfa");
    BasicBlock* check_accept_bb = b.create_block("check_accept");
    BasicBlock* non_ascii_bb = b.create_block("non_ascii_bailout");
    BasicBlock* finish_bb = b.create_block("finish");
    BasicBlock* write_match_bb = b.create_block("write_match");
    BasicBlock* ret_no_match_bb = b.create_block("ret_no_match");

    // Check if start_state < 0 or start_pos > length
    Value* state_neg = b.build_slt(start_state, gb.const_i32(0));
    Value* pos_oob = b.build_sgt(start_pos, length);
    Value* bad_entry = b.build_or(state_neg, pos_oob);
    b.build_br_if(bad_entry, early_no_match_bb, valid_start_bb);

    fn->append_block(early_no_match_bb);
    b.position_at_end(early_no_match_bb);
    b.build_ret(gb.const_i64(0));

    // Check initial state acceptance at `start_pos`
    fn->append_block(valid_start_bb);
    b.position_at_end(valid_start_bb);
    Value* start_state_64 = b.build_sext_i64(start_state);
    Value* init_acc = gb.load_i32_indexed(accept_table, start_state_64, 4, 0);
    Value* init_is_acc = b.build_sgt(init_acc, gb.const_i32(0));
    Value* neg_one = gb.const_i64(-1);
    Value* init_best_end = b.build_select(init_is_acc, start_pos, neg_one);
    Value* init_stop = b.build_eq(init_acc, gb.const_i32(2));
    b.build_br_if(init_stop, finish_bb, {init_best_end}, loop_head_bb, {start_pos, start_state, init_best_end});

    // Loop header: (cur_pos: i64, cur_state: i32, best_end: i64)
    fn->append_block(loop_head_bb);
    b.position_at_end(loop_head_bb);
    Value* cur_pos = b.add_block_param(loop_head_bb, Type::i64());
    Value* cur_state = b.add_block_param(loop_head_bb, Type::i32());
    Value* best_end = b.add_block_param(loop_head_bb, Type::i64());

    Value* has_more = b.build_slt(cur_pos, length);
    b.build_br_if(has_more, check_char_bb, {}, finish_bb, {best_end});

    // Load 16-bit UTF-16 code unit: uint16_t u = units[cur_pos]
    fn->append_block(check_char_bb);
    b.position_at_end(check_char_bb);
    Value* u32_masked = emitLoadUtf16Unit(gb, units_ptr, cur_pos);
    Value* is_non_ascii = b.build_uge(u32_masked, gb.const_i32(128));
    b.build_br_if(is_non_ascii, non_ascii_bb, step_dfa_bb);

    fn->append_block(non_ascii_bb);
    b.position_at_end(non_ascii_bb);
    b.build_ret(gb.const_i64(-1));

    // Step DFA using GrammarBuilder::dfa_table_step
    fn->append_block(step_dfa_bb);
    b.position_at_end(step_dfa_bb);
    Value* ascii_byte = b.build_and(u32_masked, gb.const_i32(0x7F));
    Value* next_state = gb.dfa_table_step(trans_table, cur_state, ascii_byte);
    Value* is_dead = b.build_slt(next_state, gb.const_i32(0));
    b.build_br_if(is_dead, finish_bb, {best_end}, check_accept_bb, {});

    // Check if `next_state` is accepting
    fn->append_block(check_accept_bb);
    b.position_at_end(check_accept_bb);
    Value* next_pos = b.build_add(cur_pos, gb.const_i64(1));
    Value* next_state_64 = b.build_sext_i64(next_state);
    Value* acc_val = gb.load_i32_indexed(accept_table, next_state_64, 4, 0);
    Value* is_acc = b.build_sgt(acc_val, gb.const_i32(0));
    Value* updated_best = b.build_select(is_acc, next_pos, best_end);
    Value* stop_now = b.build_eq(acc_val, gb.const_i32(2));
    b.build_br_if(stop_now, finish_bb, {updated_best}, loop_head_bb, {next_pos, next_state, updated_best});

    // Finish: check if `final_best >= 0`
    fn->append_block(finish_bb);
    b.position_at_end(finish_bb);
    Value* final_best = b.add_block_param(finish_bb, Type::i64());
    Value* matched = b.build_sge(final_best, gb.const_i64(0));
    b.build_br_if(matched, write_match_bb, ret_no_match_bb);

    fn->append_block(write_match_bb);
    b.position_at_end(write_match_bb);
    gb.store_i64(out_end_ptr, final_best, 0);
    b.build_ret(gb.const_i64(1));

    fn->append_block(ret_no_match_bb);
    b.position_at_end(ret_no_match_bb);
    b.build_ret(gb.const_i64(0));
}

// Emits `regex_dfa_search` into `mod` using `brass::codegen::GrammarBuilder`.
// Scans start positions `at` in `[from_pos, length]` in native machine code!
void emitDfaSearchFunction(brass::codegen::GrammarBuilder& gb, std::string_view name) {
    using namespace brass;
    Builder& b = gb.builder();
    Module* mod = b.current_module();
    Function* fn = mod->create_function(name, Type::i64(), {
        Type::ptr(), // transition_table
        Type::ptr(), // accept_table
        Type::i32(), // start_state
        Type::ptr(), // units (const uint16_t*)
        Type::i64(), // length
        Type::i64(), // from_pos
        Type::ptr(), // out_match_start (int64_t*)
        Type::ptr()  // out_match_end (int64_t*)
    });
    b.set_function(fn);

    BasicBlock* entry = b.append_block("entry");
    b.position_at_end(entry);

    Value* trans_table = b.add_block_param(entry, Type::ptr());
    Value* accept_table = b.add_block_param(entry, Type::ptr());
    Value* start_state = b.add_block_param(entry, Type::i32());
    Value* units_ptr = b.add_block_param(entry, Type::ptr());
    Value* length = b.add_block_param(entry, Type::i64());
    Value* from_pos = b.add_block_param(entry, Type::i64());
    Value* out_start_ptr = b.add_block_param(entry, Type::ptr());
    Value* out_end_ptr = b.add_block_param(entry, Type::ptr());

    BasicBlock* early_no_match_bb = b.create_block("early_no_match");
    BasicBlock* init_bb = b.create_block("init_search");
    BasicBlock* outer_head_bb = b.create_block("outer_head");
    BasicBlock* outer_body_bb = b.create_block("outer_body");
    BasicBlock* inner_head_bb = b.create_block("inner_head");
    BasicBlock* inner_char_bb = b.create_block("inner_char");
    BasicBlock* inner_step_bb = b.create_block("inner_step");
    BasicBlock* inner_acc_bb = b.create_block("inner_acc");
    BasicBlock* inner_done_bb = b.create_block("inner_done");
    BasicBlock* outer_next_bb = b.create_block("outer_next");
    BasicBlock* check_next_at_unit_bb = b.create_block("check_next_at_unit");
    BasicBlock* advance_at_bb = b.create_block("advance_at");
    BasicBlock* found_match_bb = b.create_block("found_match");
    BasicBlock* non_ascii_bb = b.create_block("non_ascii_bailout");

    Value* state_neg = b.build_slt(start_state, gb.const_i32(0));
    Value* pos_oob = b.build_sgt(from_pos, length);
    Value* bad_entry = b.build_or(state_neg, pos_oob);
    b.build_br_if(bad_entry, early_no_match_bb, init_bb);

    fn->append_block(early_no_match_bb);
    b.position_at_end(early_no_match_bb);
    b.build_ret(gb.const_i64(0));

    fn->append_block(init_bb);
    b.position_at_end(init_bb);
    Value* start_state_64 = b.build_sext_i64(start_state);
    Value* init_acc = gb.load_i32_indexed(accept_table, start_state_64, 4, 0);
    Value* init_is_acc = b.build_sgt(init_acc, gb.const_i32(0));
    Value* init_stop = b.build_eq(init_acc, gb.const_i32(2));
    Value* neg_one = gb.const_i64(-1);
    b.build_br(outer_head_bb, {from_pos});

    // Outer loop over `at` in `[from_pos, length]`
    fn->append_block(outer_head_bb);
    b.position_at_end(outer_head_bb);
    Value* at = b.add_block_param(outer_head_bb, Type::i64());
    Value* at_in_bounds = b.build_sle(at, length);
    b.build_br_if(at_in_bounds, outer_body_bb, early_no_match_bb);

    fn->append_block(outer_body_bb);
    b.position_at_end(outer_body_bb);
    Value* seed_best = b.build_select(init_is_acc, at, neg_one);
    b.build_br_if(init_stop, inner_done_bb, {seed_best}, inner_head_bb, {at, start_state, seed_best});

    // Inner loop over `cur_pos` from `at` to `length`
    fn->append_block(inner_head_bb);
    b.position_at_end(inner_head_bb);
    Value* cur_pos = b.add_block_param(inner_head_bb, Type::i64());
    Value* cur_state = b.add_block_param(inner_head_bb, Type::i32());
    Value* best_end = b.add_block_param(inner_head_bb, Type::i64());

    Value* has_more = b.build_slt(cur_pos, length);
    b.build_br_if(has_more, inner_char_bb, {}, inner_done_bb, {best_end});

    fn->append_block(inner_char_bb);
    b.position_at_end(inner_char_bb);
    Value* u32_masked = emitLoadUtf16Unit(gb, units_ptr, cur_pos);
    Value* is_non_ascii = b.build_uge(u32_masked, gb.const_i32(128));
    b.build_br_if(is_non_ascii, non_ascii_bb, inner_step_bb);

    fn->append_block(non_ascii_bb);
    b.position_at_end(non_ascii_bb);
    b.build_ret(gb.const_i64(-1));

    fn->append_block(inner_step_bb);
    b.position_at_end(inner_step_bb);
    Value* ascii_byte = b.build_and(u32_masked, gb.const_i32(0x7F));
    Value* next_state = gb.dfa_table_step(trans_table, cur_state, ascii_byte);
    Value* is_dead = b.build_slt(next_state, gb.const_i32(0));
    b.build_br_if(is_dead, inner_done_bb, {best_end}, inner_acc_bb, {});

    fn->append_block(inner_acc_bb);
    b.position_at_end(inner_acc_bb);
    Value* next_pos = b.build_add(cur_pos, gb.const_i64(1));
    Value* next_state_64 = b.build_sext_i64(next_state);
    Value* acc_val = gb.load_i32_indexed(accept_table, next_state_64, 4, 0);
    Value* is_acc = b.build_sgt(acc_val, gb.const_i32(0));
    Value* updated_best = b.build_select(is_acc, next_pos, best_end);
    Value* stop_now = b.build_eq(acc_val, gb.const_i32(2));
    b.build_br_if(stop_now, inner_done_bb, {updated_best}, inner_head_bb, {next_pos, next_state, updated_best});

    // Inner loop done: check if a match was found starting at `at`
    fn->append_block(inner_done_bb);
    b.position_at_end(inner_done_bb);
    Value* final_best = b.add_block_param(inner_done_bb, Type::i64());
    Value* matched = b.build_sge(final_best, gb.const_i64(0));
    b.build_br_if(matched, found_match_bb, outer_next_bb);

    // If `at < length`, verify that `units[at]` is ASCII before advancing `at`
    // (in case `start_state` died on the first character or before reading `units[at]`).
    fn->append_block(outer_next_bb);
    b.position_at_end(outer_next_bb);
    Value* at_lt_len = b.build_slt(at, length);
    b.build_br_if(at_lt_len, check_next_at_unit_bb, advance_at_bb);

    fn->append_block(check_next_at_unit_bb);
    b.position_at_end(check_next_at_unit_bb);
    Value* at_u32 = emitLoadUtf16Unit(gb, units_ptr, at);
    Value* at_non_ascii = b.build_uge(at_u32, gb.const_i32(128));
    b.build_br_if(at_non_ascii, non_ascii_bb, advance_at_bb);

    fn->append_block(advance_at_bb);
    b.position_at_end(advance_at_bb);
    Value* next_at = b.build_add(at, gb.const_i64(1));
    b.build_br(outer_head_bb, {next_at});

    fn->append_block(found_match_bb);
    b.position_at_end(found_match_bb);
    gb.store_i64(out_start_ptr, at, 0);
    gb.store_i64(out_end_ptr, final_best, 0);
    b.build_ret(gb.const_i64(1));
}

// Process-wide singleton holding the compiled native `KernelJit` functions for the regex DFA engine.
struct SharedRegexJitKernels {
    std::unique_ptr<brass::Module> mod;
    std::unique_ptr<brass::codegen::KernelJit> jit;
    brass::codegen::KernelFunction match_at_kf;
    brass::codegen::KernelFunction search_kf;
    RegexDfaMatchAtFn match_at_fn = nullptr;
    RegexDfaSearchFn search_fn = nullptr;

    SharedRegexJitKernels() {
        mod = RegexJitPlan::buildMirModule();
        // No SMT passes: they prove away one redundant UTF-16 mask for some
        // 200 ms of solver time, in every process that compiles a regex and
        // again at its exit, which joins this thread.
        brass::codegen::KernelOptions kopts;
        kopts.enable_optimizations = true;
        kopts.enable_smt_guard_elim = false;
        kopts.enable_superopt = false;
        kopts.enable_unroll = false;
        kopts.enable_vectorize = false;
        jit = std::make_unique<brass::codegen::KernelJit>(kopts);

        match_at_kf = jit->compile(*mod, "regex_dfa_match_at");
        search_kf = jit->compile(*mod, "regex_dfa_search");
        match_at_fn = match_at_kf.as<RegexDfaMatchAtFn>();
        search_fn = search_kf.as<RegexDfaSearchFn>();
    }
};

// Built once, on a thread of its own (RegexJitPlan::waitForKernels): the first
// match/search (or waitForKernels) starts the background kernel build so the
// caller never blocks on it and a process that only parses regexes never
// compiles them. Leaked at exit so its destructor never runs inside
// DllMain(DLL_PROCESS_DETACH) under the Windows loader lock; at-exit teardown
// cancels any still-pending build and waitForPendingKernels() joins the worker
// before static teardown begins.
struct KernelBuild {
    std::atomic<const SharedRegexJitKernels*> ready{nullptr};
    std::atomic<bool> canceled{false};
    std::unique_ptr<SharedRegexJitKernels> kernels;
    std::mutex mu, joinMu;
    std::condition_variable cv;
    bool done = false;
    std::thread worker;

    KernelBuild() {
        worker = std::thread([this] {
            std::unique_ptr<SharedRegexJitKernels> k;
            if (!canceled.load(std::memory_order_acquire)) {
                k = std::make_unique<SharedRegexJitKernels>();
            }
            std::lock_guard<std::mutex> lock(mu);
            if (k) {
                kernels = std::move(k);
                ready.store(kernels.get(), std::memory_order_release);
            }
            done = true;
            cv.notify_all();
        });
    }
    void waitAndJoin(bool cancelIfPending = false) {
        if (brass::runtime::process_exiting()) return;
        {
            std::unique_lock<std::mutex> lock(mu);
            if (cancelIfPending) canceled.store(true, std::memory_order_release);
            cv.wait(lock, [this] { return done; });
        }
        std::lock_guard<std::mutex> joinLock(joinMu);
        if (worker.joinable()) worker.join();
    }
};

static std::atomic<KernelBuild*> g_kernelBuild{nullptr};

void atExitWaitForKernels() {
    if (KernelBuild* b = g_kernelBuild.load(std::memory_order_acquire)) b->waitAndJoin(true);
}

KernelBuild& kernelBuild() {
    static KernelBuild* build = [] {
        auto* b = new KernelBuild();
        g_kernelBuild.store(b, std::memory_order_release);
        std::atexit(&atExitWaitForKernels);
        return b;
    }();
    return *build;
}

// The kernels, or null while they are still being built (started here).
const SharedRegexJitKernels* readyKernels() { return kernelBuild().ready.load(std::memory_order_acquire); }

}  // namespace

std::unique_ptr<brass::Module> RegexJitPlan::buildMirModule(const int32_t* transition_table, int32_t num_states) {
    auto mod = std::make_unique<brass::Module>("bronze_regex_dfa_jit");
    brass::codegen::GrammarBuilder gb(*mod);

    // 1. Emit the table-driven single-step function via GrammarBuilder
    gb.build_dfa_table_step_function("regex_dfa_table_step");

    // 2. If a concrete transition table is provided, also emit the specialized immediate-table step kernel
    if (transition_table != nullptr && num_states > 0) {
        gb.build_dfa_step_function(transition_table, num_states, "regex_dfa_step_specialized");
    }

    // 3. Emit the unrolled byte string scanner and UTF-16 step kernel via GrammarBuilder
    gb.build_dfa_scan_string_function("regex_dfa_scan_bytes", 1);
    emitDfaUtf16StepFunction(gb, "regex_dfa_utf16_step");

    // 4. Emit the UTF-16 `matchAt` and `search` DFA kernels
    emitDfaMatchAtFunction(gb, "regex_dfa_match_at");
    emitDfaSearchFunction(gb, "regex_dfa_search");

    for (brass::Function* fn : mod->functions()) {
        if (fn) fn->rebuild_cfg_predecessors();
    }
    return mod;
}

namespace {
void fillJitCaptures(int64_t matchStart, int64_t matchEnd,
                     const std::vector<FixedCaptureSpec>& fixed, MatchResult& out) {
    out.captures.resize(2 + fixed.size() * 2);
    int64_t* dst = out.captures.data();
    dst[0] = matchStart;
    dst[1] = matchEnd;
    for (size_t i = 0; i < fixed.size(); ++i) {
        dst[2 + i * 2] = fixed[i].start.eval(matchStart, matchEnd);
        dst[2 + i * 2 + 1] = fixed[i].end.eval(matchStart, matchEnd);
    }
}
}  // namespace

std::unique_ptr<RegexJitPlan> RegexJitPlan::tryCompile(const Pattern& pattern) {
    if (!pattern.root) return nullptr;

    jit_dfa::NfaCompiler nfa_compiler(pattern.flags);
    bool anchors_at_start = false;
    if (!nfa_compiler.compileRoot(*pattern.root, anchors_at_start)) return nullptr;

    auto plan = std::make_unique<RegexJitPlan>();
    if (pattern.groupCount > 0) {
        plan->requires_capture_extraction_ = !jit_dfa::tryResolveFixedAsciiCaptures(
            *pattern.root, pattern.groupCount, plan->fixed_captures_);
    }
    plan->anchors_at_start_ = anchors_at_start;
    plan->stats_.nfa_states = static_cast<uint32_t>(nfa_compiler.insts().size());

    plan->transition_table_.assign(256, -1);
    jit_dfa::DfaBuilder dfa_builder(nfa_compiler.insts());
    if (!dfa_builder.build(nfa_compiler.startState(), plan->transition_table_, plan->accept_table_,
                           plan->start_state_, plan->num_dfa_states_)) {
        return nullptr;
    }

    plan->stats_.dfa_states = static_cast<uint32_t>(plan->num_dfa_states_);
    plan->supported_ = true;
    return plan;
}

void RegexJitPlan::waitForKernels() { kernelBuild().waitAndJoin(false); }

void RegexJitPlan::waitForPendingKernels() {
    if (KernelBuild* b = g_kernelBuild.load(std::memory_order_acquire)) b->waitAndJoin(false);
}

RegexJitStats RegexJitPlan::stats() const noexcept { return stats_; }

bool RegexJitPlan::tryMatchAt(UnitsView input, size_t at, MatchResult& out, ExecStatus& status) const {
    const SharedRegexJitKernels* k = supported_ ? readyKernels() : nullptr;
    if (!k || !k->match_at_fn) return false;
    if (at > input.size() || (anchors_at_start_ && at != 0) || start_state_ < 0 || num_dfa_states_ <= 0) {
        status = ExecStatus::NoMatch;
        return true;
    }

    int64_t match_end = -1;
    const uint16_t* raw_units = input.empty() ? nullptr : reinterpret_cast<const uint16_t*>(input.data());
    const int64_t rc = k->match_at_fn(
        transition_table_.data(), accept_table_.data(), start_state_,
        raw_units, static_cast<int64_t>(input.size()), static_cast<int64_t>(at), &match_end);

    if (rc < 0) return false; // Non-ASCII code unit encountered; fall back to full UTF-16 interpreter.
    if (rc == 0) {
        status = ExecStatus::NoMatch;
        return true;
    }
    fillJitCaptures(static_cast<int64_t>(at), match_end, fixed_captures_, out);
    status = ExecStatus::Match;
    return true;
}

bool RegexJitPlan::trySearch(UnitsView input, size_t from, MatchResult& out, ExecStatus& status) const {
    const SharedRegexJitKernels* k = supported_ ? readyKernels() : nullptr;
    if (!k || !k->search_fn) return false;
    if (from > input.size() || start_state_ < 0 || num_dfa_states_ <= 0) {
        status = ExecStatus::NoMatch;
        return true;
    }
    if (anchors_at_start_) {
        if (from > 0) {
            status = ExecStatus::NoMatch;
            return true;
        }
        return tryMatchAt(input, 0, out, status);
    }

    int64_t match_start = -1;
    int64_t match_end = -1;
    const uint16_t* raw_units = input.empty() ? nullptr : reinterpret_cast<const uint16_t*>(input.data());
    const int64_t rc = k->search_fn(
        transition_table_.data(), accept_table_.data(), start_state_,
        raw_units, static_cast<int64_t>(input.size()), static_cast<int64_t>(from),
        &match_start, &match_end);

    if (rc < 0) return false; // Non-ASCII code unit encountered; fall back to full UTF-16 interpreter.
    if (rc == 0) {
        status = ExecStatus::NoMatch;
        return true;
    }
    fillJitCaptures(match_start, match_end, fixed_captures_, out);
    status = ExecStatus::Match;
    return true;
}

bool hasJitFastPath(const Pattern& pattern) noexcept {
    return pattern.jitPlan != nullptr && pattern.jitPlan->isSupported();
}

}  // namespace bronze::regex
