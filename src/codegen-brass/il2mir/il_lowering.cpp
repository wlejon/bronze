#include "il_lowering.h"
#include "il_lowering_dynamic.h"
#include "il_call_entry.h"
#include "il_prop_stubs.h"
#include "il_lowering_coro.h"
#include "il_lowering_ops.h"
#include "il_lowering_calls.h"
#include "il_abi.h"
#include "il_bridge_verifier.h"
#include "il_pipeline.h"
#include <brass/mir/coro_transform.hpp>
#include <brass/mir/printer.hpp>
#include <brass/mir/verifier.hpp>
#include "support/timings.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace il2mir {

std::string IlLowering::resolve_callee(const std::string& callee_name) const {
    auto it_fn = caller_to_callee_map_.find(current_fn_idx_);
    if (it_fn != caller_to_callee_map_.end()) {
        auto it_name = it_fn->second.find(callee_name);
        if (it_name != it_fn->second.end()) {
            return it_name->second;
        }
    }
    return callee_name;
}

std::string IlLowering::resolve_create_func_callee(const std::string& callee_name) {
    auto it_caller = caller_create_func_targets_.find(current_fn_idx_);
    if (it_caller != caller_create_func_targets_.end()) {
        auto it_targets = it_caller->second.find(callee_name);
        if (it_targets != it_caller->second.end() && !it_targets->second.empty()) {
            size_t k = create_func_counter_[callee_name]++;
            if (k < it_targets->second.size()) {
                return it_targets->second[k];
            }
        }
    }
    return resolve_callee(callee_name);
}

Value* IlLowering::get_key_id(Builder& b, uint32_t key_idx) {
    Value* map_addr = b.build_func_addr(module_sym("__bronze_key_map"));
    return b.build_load(Type::i32(), map_addr, static_cast<int32_t>(key_idx * sizeof(uint32_t)));
}

Value* IlLowering::thread_tls_block(Builder& b) {
    Module* mod = b.current_block()->parent()->parent();
    return (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                               : b.build_call("bronze_tls_block_addr", Type::i64(), {});
}

// cell = tls->global_read_cells + key * stride, valid while key < the count
// and cell.epoch == tls->global_read_epoch; the value is *cell.read, and the
// hole there is a name that resolves to nothing. Six loads and three
// compares on a hit; everything else is the runtime's.
Value* IlLowering::lower_global_name_read(Builder& b, Value* key, bool soft) {
    Value* tls = thread_tls_block(b);
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "gname_" + std::to_string(fn->next_block_id());
    BasicBlock* in_table = b.append_block(prefix + "_in");
    BasicBlock* valid = b.append_block(prefix + "_valid");
    BasicBlock* miss = b.append_block(prefix + "_miss_cold");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());
    b.position_at_end(cur);

    Value* key64 = b.build_zext_i64(key);
    Value* count = b.build_load(Type::i64(), tls, BRONZE_TLS_GLOBAL_READ_CELL_COUNT_OFF);
    b.build_br_if(b.build_ult(key64, count), in_table, {}, miss, {});

    b.position_at_end(in_table);
    Value* cells = b.build_load(Type::i64(), tls, BRONZE_TLS_GLOBAL_READ_CELLS_OFF);
    Value* cell = b.build_add(cells, b.build_shl(key64, b.build_iconst_i64(6)));
    static_assert(BRONZE_ABI_GLOBAL_READ_CELL_STRIDE == 64);
    Value* cell_epoch = b.build_load(Type::i64(), cell, BRONZE_ABI_GLOBAL_READ_CELL_EPOCH);
    Value* epoch = b.build_load(Type::i64(), tls, BRONZE_TLS_GLOBAL_READ_EPOCH_OFF);
    b.build_br_if(b.build_eq(cell_epoch, epoch), valid, {}, miss, {});

    b.position_at_end(valid);
    Value* read = b.build_load(Type::i64(), cell, BRONZE_ABI_GLOBAL_READ_CELL_READ);
    Value* v = b.build_load(Type::i64(), read, 0);
    Value* hole = b.build_eq(v, b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_HOLE_BITS)));
    if (soft) {
        b.build_br(merge, {b.build_select(hole, b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_UNDEFINED_BITS)), v)});
    } else {
        b.build_br_if(hole, miss, {}, merge, {v});
    }

    b.position_at_end(miss);
    Value* slow = b.build_call("bronze_resolve_name", Type::i64(), {key, b.build_iconst_i32(soft ? 1 : 0)});
    b.build_br(merge, {slow});

    b.position_at_end(merge);
    return result;
}

BasicBlock* IlLowering::lower_global_name_write(Builder& b, Value* key, Value* val) {
    Value* tls = thread_tls_block(b);
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "gput_" + std::to_string(fn->next_block_id());
    BasicBlock* in_table = b.append_block(prefix + "_in");
    BasicBlock* valid = b.append_block(prefix + "_valid");
    BasicBlock* store = b.append_block(prefix + "_store");
    BasicBlock* miss = b.append_block(prefix + "_miss");
    miss->set_cold();
    BasicBlock* done = b.append_block(prefix + "_done");
    b.position_at_end(cur);

    Value* key64 = b.build_zext_i64(key);
    Value* count = b.build_load(Type::i64(), tls, BRONZE_TLS_GLOBAL_READ_CELL_COUNT_OFF);
    b.build_br_if(b.build_ult(key64, count), in_table, {}, miss, {});

    b.position_at_end(in_table);
    Value* cells = b.build_load(Type::i64(), tls, BRONZE_TLS_GLOBAL_READ_CELLS_OFF);
    Value* cell = b.build_add(cells, b.build_shl(key64, b.build_iconst_i64(6)));
    Value* cell_epoch = b.build_load(Type::i64(), cell, BRONZE_ABI_GLOBAL_READ_CELL_EPOCH);
    Value* epoch = b.build_load(Type::i64(), tls, BRONZE_TLS_GLOBAL_READ_EPOCH_OFF);
    b.build_br_if(b.build_eq(cell_epoch, epoch), valid, {}, miss, {});

    b.position_at_end(valid);
    Value* write = b.build_load(Type::i64(), cell, BRONZE_ABI_GLOBAL_READ_CELL_WRITE);
    b.build_br_if(b.build_ne(write, b.build_iconst_i64(0)), store, {}, miss, {});

    b.position_at_end(store);
    prop_lowering_.store_value(b, write, val, kObjectSlotAliasClass, /*need_barrier=*/true);
    b.build_br(done, {});

    b.position_at_end(miss);
    return done;
}

// tls->module_deltas[*__bronze_module_slot]: the thread's delta for this
// module, valid once the thread has run the module's entry (which registered
// it). Three loads, the first two independent.
Value* IlLowering::load_module_delta(Builder& b) {
    Module* mod = b.current_block()->parent()->parent();
    Value* tls = (mod && mod->pinned_tls_register())
                     ? b.build_pinned_tls_read()
                     : b.build_call("bronze_tls_block_addr", Type::i64(), {});
    Value* deltas = b.build_load(Type::i64(), tls, kBronzeTlsModuleDeltasOff);
    Value* slot = b.build_load(Type::i64(), b.build_func_addr(module_sym("__bronze_module_slot")), 0);
    // One scaled-index load: `mov rax, [deltas + slot*8]`.
    return b.build_load_indexed(Type::i64(), deltas, slot, 8);
}

Value* IlLowering::module_delta(Builder& b) {
    if (current_module_delta_) return current_module_delta_;
    return load_module_delta(b);
}

Value* IlLowering::module_data_addr(Builder& b, const std::string& base) {
    Value* addr = b.build_func_addr(module_sym(base));
    if (!options_.per_thread_module_data) return addr;
    return b.build_add(addr, module_delta(b));
}

Value* IlLowering::alloc_memento(Builder& b, uint32_t site) {
    if (!options_.cache_evidence || site == BronzeInstruction::kNoSlowSite || site + kAllocTenuredArm >= options_.slow_site_count) {
        return nullptr;
    }
    const auto word = [&](uint32_t index) {
        if (options_.slow_armed) options_.slow_armed->mark(index);
        return b.build_add(b.build_func_addr(module_sym("__bronze_ic_table")),
                           b.build_iconst_i64(ic_slow_count_offset(index, options_.slow_site_count)));
    };
    Value* count = word(site + kAllocCountArm);
    Value* n = with_alias_class(b.build_load(Type::i64(), count, 0), kSlowCountAliasClass);
    with_alias_class(b.build_store(Type::i64(), count, 0, b.build_add(n, b.build_iconst_i64(1))), kSlowCountAliasClass);
    return word(site + kAllocTenuredArm);
}

Value* IlLowering::pretenure_site(Builder& b, uint32_t site) {
    const std::vector<uint32_t>& sites = options_.pretenure_sites;
    const auto it = std::lower_bound(sites.begin(), sites.end(), site);
    if (site == BronzeInstruction::kNoSlowSite || it == sites.end() || *it != site) return nullptr;
    const int64_t at = static_cast<int64_t>(it - sites.begin()) * static_cast<int64_t>(kPretenureSiteBytes);
    return b.build_add(b.build_func_addr(module_sym(std::string(kPretenureSitesSymbol))), b.build_iconst_i64(at));
}

uint32_t IlLowering::find_key_constant(const std::string& name) const {
    for (size_t i = 0; i < options_.key_constants.size(); ++i) {
        if (options_.key_constants[i] == name) return static_cast<uint32_t>(i);
    }
    return 0;
}

Value* IlLowering::get_val_by_id(uint32_t id, Builder& b, const std::unordered_map<uint32_t, Value*>& val_map) {
    if (module_env_regs_.count(id)) {
        Value* env_addr = module_data_addr(b, "__bronze_module_env");
        return b.build_load(Type::i64(), env_addr, 0);
    }
    auto it = val_map.find(id);
    if (it == val_map.end()) return nullptr;
    // A dynamic value lives as a tagged SSA value, which every tier reports
    // to the collector; each use reads its bits afresh, so a use after a
    // collection sees where the object is now.
    Value* result = it->second;
    if (result && result->type().is_tagged()) return unbox_tagged(result, b);
    return result;
}

void IlLowering::set_inst_result(uint32_t result_id, Value* res_val, Builder& b, std::unordered_map<uint32_t, Value*>& val_map) {
    if (result_id == UINT32_MAX || !res_val) return;
    if (current_fn_dynamic_ids_.count(result_id) && !res_val->type().is_void()) {
        res_val = ensure_type(res_val, Type::tagged(), b);
    }
    val_map[result_id] = res_val;
}

Value* IlLowering::stage_argv(Builder& b, const std::vector<Value*>& args) {
    if (args.empty()) return b.build_iconst_i64(0);
    if (argv_block_ == nullptr || args.size() > argv_block_words_) {
        has_error_ = true;
        if (diag_) diag_->error(SourceLocation("", 0, 0),"il2mir: an argv block too small for a call's arguments");
        return b.build_iconst_i64(0);
    }
    for (size_t i = 0; i < args.size(); ++i) {
        b.build_store(Type::i64(), argv_block_, static_cast<int32_t>(i * 8), ensure_type(args[i], Type::i64(), b));
    }
    return argv_block_;
}

// The order the blocks of one function are lowered in: reverse post-order of
// its CFG, then any block the walk did not reach, in list order.
//
// Lowering looks every operand up in `val_map` as it goes, so a use has to be
// lowered after its definition. The IL only promises SSA dominance, not that
// the block list is laid out in dominance order: lowering an `if` without an
// `else` inside a loop appends the loop's continue block before the join block
// that defines what the continue block forwards (`if (d === 0) x = c; n = n *
// 10;` with a `return` in the other arm), and a use lowered first found no
// value and failed the MIR verifier with "operand is null". A dominator comes
// before everything it dominates in any reverse post-order, so that order is
// always sound. The exception handler of a block counts as its successor, since
// that edge is as real as a branch's. The same walk finds the LOOP HEADERS a
// build made to be harvested counts (il/feedback_site.h kLoopCountOffset):
// the targets of its back edges, the edges to a block still on its stack;
// `headers` gets one flag per block, by index.
static std::vector<const BronzeBlock*> lowering_order(const BronzeFunction& fn_ast, std::vector<uint8_t>& headers) {
    std::vector<const BronzeBlock*> order;
    headers.assign(fn_ast.blocks.size(), 0);
    if (fn_ast.blocks.empty()) return order;
    std::unordered_map<uint32_t, size_t> index_of;
    for (size_t i = 0; i < fn_ast.blocks.size(); ++i) index_of.emplace(fn_ast.blocks[i].id, i);

    auto successors = [&](const BronzeBlock& blk) {
        std::vector<size_t> out;
        auto add = [&](uint32_t id) {
            if (const auto it = index_of.find(id); it != index_of.end()) out.push_back(it->second);
        };
        for (const auto& inst : blk.instructions) {
            if (inst.op == BronzeOp::Jump) add(inst.target.block_id);
            if (inst.op == BronzeOp::Branch) {
                add(inst.target.block_id);
                add(inst.else_target.block_id);
            }
        }
        if (blk.handler_id != UINT32_MAX) add(blk.handler_id);
        return out;
    };

    // Iterative DFS: a large function has thousands of blocks, too many for
    // the native stack. `visited`: 1 while on the stack, 2 once finished.
    std::vector<uint8_t> visited(fn_ast.blocks.size(), 0);
    std::vector<size_t> post;
    post.reserve(fn_ast.blocks.size());
    std::vector<std::pair<size_t, std::vector<size_t>>> stack;
    visited[0] = 1;
    stack.emplace_back(0, successors(fn_ast.blocks[0]));
    while (!stack.empty()) {
        auto& [node, succ] = stack.back();
        if (succ.empty()) {
            visited[node] = 2;
            post.push_back(node);
            stack.pop_back();
            continue;
        }
        const size_t next = succ.front();
        succ.erase(succ.begin());
        if (visited[next] == 1) headers[next] = 1;
        if (visited[next]) continue;
        visited[next] = 1;
        stack.emplace_back(next, successors(fn_ast.blocks[next]));
    }
    for (auto it = post.rbegin(); it != post.rend(); ++it) order.push_back(&fn_ast.blocks[*it]);
    for (size_t i = 0; i < fn_ast.blocks.size(); ++i) {
        if (!visited[i]) order.push_back(&fn_ast.blocks[i]);
    }
    return order;
}

bool IlLowering::lower_function(const BronzeFunction& fn_ast, Module& mod, const std::string& fn_name,
                                Function* target) {
    Function* fn = target ? target : mod.get_function(fn_name);
    if (!fn) return false;

    module_env_regs_.clear();
    current_fn_dynamic_ids_.clear();
    key_boxes_.clear();
    create_func_counter_.clear();
    argv_block_ = nullptr;
    argv_block_words_ = 0;
    leave_state_ = nullptr;
    leave_state_words_ = 0;
    il_types_built_ = false;
    current_module_delta_ = nullptr;
    analyze_function(fn_ast);
    record_inline_scopes(fn_ast, *fn->parent());

    // The argv block a call stages its arguments in (il_lowering_calls.h),
    // as wide as this function's own calls need: a twin makes its own
    // (il_lowering_resume.cpp). The state of the leaves and resume.calls
    // that are calls goes into one buffer as wide as the widest. One buffer
    // serves them all: a resume.call's twin reads its state at entry, before
    // anything after it in the exit (another resume.call, the leave) stores
    // into the buffer again.
    const uint32_t widest_method_argc = argv_block_words(this, fn_ast);
    uint32_t widest_leave = 0;
    for (const auto& blk : fn_ast.blocks) {
        for (const auto& inst : blk.instructions) {
            if ((inst.op != BronzeOp::Leave && inst.op != BronzeOp::ResumeCall) ||
                inst.index >= current_ast_->functions.size()) {
                continue;
            }
            // A leave that is a guard: its twin reads the state from the guard (il_lowering_exit.cpp).
            if (inst.op == BronzeOp::Leave && leave_target(inst, *fn) != nullptr) continue;
            const uint32_t words = static_cast<uint32_t>(inst.operands.size()) +
                                   (options_.per_thread_module_data ? 1u : 0u);
            widest_leave = std::max(widest_leave, words);
        }
    }

    Builder b(mod);
    b.set_function(fn);
    // The lowering spells every tag, offset and limit inline; one of each per
    // block is enough.
    b.set_const_reuse(true);

    std::unordered_map<uint32_t, BasicBlock*> block_map;
    std::unordered_map<uint32_t, Value*> val_map;

    // 1. Create all basic blocks in advance
    for (const auto& blk_ast : fn_ast.blocks) {
        std::string bb_name = "b" + std::to_string(blk_ast.id);
        BasicBlock* bb = b.append_block(bb_name);
        block_map[blk_ast.id] = bb;
    }

    // 2. Set up entry block parameters from function arguments
    coro_frame_val_ = nullptr;
    coro_suspend_count_ = 0;
    if (!fn_ast.blocks.empty() && block_map.count(fn_ast.blocks[0].id)) {
        BasicBlock* entry_bb = block_map[fn_ast.blocks[0].id];
        if (fn_ast.is_coroutine_body()) coro_frame_val_ = b.add_block_param(entry_bb, Type::gcref());
        std::vector<Value*> entry_params;
        for (size_t i = 0; i < fn_ast.params.size(); ++i) {
            uint32_t param_id = fn_ast.params[i].first;
            Type param_type = lower_abi_type(fn_ast.params[i].second);
            Value* param_val = b.add_block_param(entry_bb, param_type);
            val_map[param_id] = param_val;
            entry_params.push_back(param_val);
        }

        b.position_at_end(entry_bb);
        // In the entry block itself, ahead of the stack check's split: a
        // coroutine body's allocas must be there to be re-created on every
        // resume (brass's coroutine lowering refuses any other).
        if (widest_method_argc > 0) {
            argv_block_ = b.build_alloca_tagged(widest_method_argc);
            // Scratch: a resume twin stages its calls in a block of its own.
            argv_block_->defining_instruction()->set_fresh_in_twin();
            argv_block_words_ = widest_method_argc;
        }
        if (widest_leave > 0) {
            leave_state_ = b.build_alloca_tagged(widest_leave);
            leave_state_words_ = widest_leave;
        }
        if (options_.pin_tls_register) {
            // The module entry is the one function the runtime calls without
            // its trampoline (bro and the CLI call the exported symbol
            // directly), so it fetches the block itself. Every other
            // function arrives with the register set by its caller: a
            // compiled caller never touches it, a runtime caller went
            // through `bronze_enter_js`. A coroutine body is the other
            // exception: brass's resume runs it from C++, so it fetches the
            // block here and again after every suspension — the plain
            // address, since only the module entry arms the stack limit
            // (bronze_tls_enter re-arms it below the current frame, which a
            // body entered once per nesting level would ratchet off the
            // bottom of the stack).
            if (fn_name == "main") {
                Value* tls = b.build_call("bronze_tls_enter", Type::i64(), {});
                b.build_pinned_tls_write(tls);
            } else if (fn_ast.is_coroutine_body()) {
                Value* tls = b.build_call("bronze_tls_block_addr", Type::i64(), {});
                b.build_pinned_tls_write(tls);
            }
            // Stack-limit check, before anything is pushed: below the limit,
            // the runtime raises the RangeError, which leaves this frame as
            // any throw from a call does. The limit may instead be an armed
            // interrupt (a compartment's time slice, runtime/tls_block.h):
            // then the runtime services it and returns, and the function
            // runs on.
            Value* tls = b.build_pinned_tls_read();
            Value* limit = b.build_load(Type::i64(), tls, kBronzeTlsStackLimitOff);
            Value* sp = b.build_read_sp();
            Value* below = b.build_ult(sp, limit);
            // append_block moves the insertion point, so come back to the
            // entry for the branch.
            BasicBlock* overflow_bb = b.append_block("stack_overflow");
            BasicBlock* body_bb = b.append_block("stack_ok");
            b.position_at_end(entry_bb);
            b.build_br_if(below, overflow_bb, body_bb);
            b.position_at_end(overflow_bb);
            b.build_call("bronze_stack_overflow", Type::void_type(), {});
            b.build_br(body_bb);
            // The IL's first block now lowers into the checked continuation;
            // its parameters stay on the real entry block, which dominates it.
            block_map[fn_ast.blocks[0].id] = body_bb;
            b.position_at_end(body_bb);
            stack_check_entry_bb_ = entry_bb;
            stack_check_overflow_bb_ = overflow_bb;
            stack_check_body_bb_ = body_bb;
        }
        if (options_.per_thread_module_data) {
            // The module's per-thread data (TranslatorOptions::
            // per_thread_module_data). The entry registers this thread's
            // instance before anything can touch a table; every other
            // function reads the delta once here, and an unused one is dead
            // code the optimizer drops.
            if (fn_name == "main") {
                current_module_delta_ = b.build_call("bronze_module_instance", Type::i64(), {
                    b.build_func_addr(module_sym("__bronze_module_slot")),
                    b.build_func_addr(module_sym("__bronze_instance")),
                    b.build_func_addr(module_sym("__bronze_instance_end"))});
            } else {
                current_module_delta_ = load_module_delta(b);
            }
        }
        if (fn_name == "main") {
            // The module's code ranges first, before anything here can
            // allocate: their stack maps are how a collection finds the
            // Values this module's frames hold. However a host brought the
            // code in (linked, loaded, JIT), the entry is the one thing it
            // must run; a second registration of a table is a no-op.
            Value* ranges_addr = b.build_func_addr(code_ranges_symbol(options_.entry_symbol));
            Value* range_count = b.build_load(Type::i32(), b.build_func_addr(code_range_count_symbol(options_.entry_symbol)), 0);
            b.build_call("bronze_register_code_ranges", Type::void_type(), {ranges_addr, range_count});
            Value* env_addr = module_data_addr(b, "__bronze_module_env");
            Value* count_val = b.build_iconst_i64(1);
            b.build_call("bronze_register_value_cells", Type::void_type(), {env_addr, count_val});
            Value* tpl_addr = module_data_addr(b, "__bronze_template_cells");
            const size_t tpl_count = std::max<size_t>(1024, static_cast<size_t>(options_.template_site_count) + 128);
            Value* tpl_cells_count = b.build_iconst_i64(static_cast<int64_t>(tpl_count));
            // Not value cells: only bronze_template_object stores into
            // these, so a minor visits just the ones it filled young.
            b.build_call("bronze_register_template_cells", Type::void_type(), {tpl_addr, tpl_cells_count});
            const std::string key_sym = (options_.entry_symbol.empty() || options_.entry_symbol == "main" || options_.entry_symbol == "bronze_main")
                                            ? "bronze_main_key_constants"
                                            : (options_.entry_symbol + "_key_constants");
            Value* manifest_addr = b.build_func_addr(key_sym);
            Value* map_addr = b.build_func_addr(module_sym("__bronze_key_map"));
            b.build_call("bronze_register_key_manifest", Type::void_type(), {manifest_addr, map_addr});
            // The method-call sites' env words, registered as value cells
            // (bronze_abi.h, bronze_register_method_ic_cells): a latched
            // direct-form entry may hold a closure's environment record —
            // a heap Value in module data — and only registration keeps it
            // current across a collection.
            if (options_.ic_site_count > 0 && !options_.method_ic_sites.empty()) {
                Value* table_addr = module_data_addr(b, "__bronze_ic_table");
                Value* sites_addr = b.build_func_addr(module_sym("__bronze_method_ic_sites"));
                Value* site_count = b.build_iconst_i64(static_cast<int64_t>(options_.method_ic_sites.size()));
                b.build_call("bronze_register_method_ic_cells", Type::void_type(), {table_addr, sites_addr, site_count});
            }
            // The sites' packed way-0 rows (bronze_abi_layout.h, "the HOT
            // array"), which the way-0 guards read and only the runtime's
            // fills keep current: registered before any site can fill. A
            // module with no sites registers too: the range is what its
            // harvest's slow counts and samples are folded through.
            {
                Value* table_addr = module_data_addr(b, "__bronze_ic_table");
                Value* hot_addr = b.build_add(table_addr, b.build_iconst_i64(ic_hot_table_offset(options_.ic_site_count)));
                Value* site_count = b.build_iconst_i64(static_cast<int64_t>(options_.ic_site_count));
                Value* identity_addr = b.build_func_addr(module_sym("__bronze_ic_identity"));
                b.build_call("bronze_register_ic_hot", Type::void_type(),
                             {table_addr, hot_addr, site_count, identity_addr});
            }
            // The deopt counters (TranslatorOptions::deopt_generic) with the
            // text naming each, for the report at exit.
            if (options_.deopt_site_count > 0) {
                Value* counts_addr = module_data_addr(b, "__bronze_deopt_counts");
                Value* text_addr = b.build_func_addr(module_sym("__bronze_deopt_sites"));
                b.build_call("bronze_register_deopt_counts", Type::void_type(),
                             {counts_addr, text_addr, b.build_iconst_i32(static_cast<int32_t>(options_.deopt_generic_count)),
                              b.build_iconst_i32(static_cast<int32_t>(options_.deopt_site_count)),
                              b.build_iconst_i64(static_cast<int64_t>(options_.deopt_retire_after)),
                              b.build_iconst_i64(static_cast<int64_t>(options_.deopt_optimistic_after))});
            }
            // The method-call sites' rows (bronze_abi_layout.h, "the METHOD
            // ROWS"), kept by the runtime's latches the same way.
            if (options_.ic_site_count > 0 && !options_.method_ic_sites.empty()) {
                Value* table_addr = module_data_addr(b, "__bronze_ic_table");
                Value* rows_addr =
                    b.build_add(table_addr, b.build_iconst_i64(ic_method_rows_offset(options_.ic_site_count)));
                Value* sites_addr = b.build_func_addr(module_sym("__bronze_method_ic_sites"));
                Value* site_count = b.build_iconst_i64(static_cast<int64_t>(options_.method_ic_sites.size()));
                b.build_call("bronze_register_method_ic_rows", Type::void_type(),
                             {table_addr, rows_addr, sites_addr, site_count});
            }
            // The keys eligible for a double slot (il.h
            // Module::numberSlotKeys), before the cells are resolved and
            // before any object this module makes gets its shape.
            if (!options_.number_slot_keys.empty()) {
                Value* keys_addr = b.build_func_addr(module_sym(std::string(kNumberSlotKeysSymbol)));
                Value* key_count = b.build_iconst_i32(static_cast<int32_t>(options_.number_slot_keys.size()));
                b.build_call("bronze_register_slot_repr", Type::void_type(), {keys_addr, key_count, map_addr});
            }
            // The module's shape cells (il.h Module::shapeCells): the
            // runtime fills each with the shape its key names, now for one
            // that exists and when one is made for the rest, before `shape.cell`
            // reads any (il_abi.h kShapeCellsSymbol).
            if (options_.shape_cell_count > 0) {
                Value* cells_addr = module_data_addr(b, std::string(kShapeCellsSymbol));
                Value* keys_addr = b.build_func_addr(module_sym(std::string(kShapeCellKeysSymbol)));
                Value* cell_count = b.build_iconst_i32(static_cast<int32_t>(options_.shape_cell_count));
                b.build_call("bronze_register_shape_cells", Type::void_type(), {cells_addr, keys_addr, cell_count});
            }
            if (options_.enable_census && options_.census_site_count > 0) {
                Value* out_path = b.build_func_addr(module_sym("__bronze_census_out_path"));
                Value* sites_addr = b.build_func_addr(module_sym("__bronze_census_sites"));
                Value* site_count = b.build_iconst_i32(static_cast<int32_t>(options_.census_site_count));
                b.build_call("bronze_census_register", Type::void_type(), {out_path, sites_addr, site_count, map_addr});
            }
            for (size_t f = 0; f < options_.source_files.size(); ++f) {
                if (options_.source_files[f].entry_count == 0) continue;
                Value* text_addr = b.build_func_addr(module_sym("__bronze_source_text_" + std::to_string(f)));
                Value* text_len = b.build_iconst_i32(static_cast<int32_t>(options_.source_files[f].text_len));
                Value* entries_addr = b.build_func_addr(module_sym("__bronze_source_entries_" + std::to_string(f)));
                Value* entries_count = b.build_iconst_i32(static_cast<int32_t>(options_.source_files[f].entry_count));
                b.build_call("bronze_register_fn_sources", Type::void_type(), {text_addr, text_len, entries_addr, entries_count});
            }
        }
        // The entry count of a build made to be harvested (il/feedback_site.h
        // Function::entrySlowSite); a coroutine body's entry block runs again
        // at every resume, which counts each stretch it runs.
        if (fn_ast.entry_slow_site != UINT32_MAX) prop_lowering_.count_index(b, fn_ast.entry_slow_site);
        // A cold function's entries (TranslatorOptions::cold_entry_count).
        if (fn_ast.cold_entry < options_.cold_entry_count) {
            Value* at = b.build_add(module_data_addr(b, std::string(kColdEntriesSymbol)),
                                    b.build_iconst_i64(static_cast<int64_t>(fn_ast.cold_entry) * 8));
            Value* n = with_alias_class(b.build_load(Type::i64(), at, 0), kSlowCountAliasClass);
            with_alias_class(b.build_store(Type::i64(), at, 0, b.build_add(n, b.build_iconst_i64(1))),
                             kSlowCountAliasClass);
        }
    }

    // 3. Set up non-entry block parameters
    // A Boolean one is an i32 that is 0 or 1 (zero_one_params_).
    zero_one_params_.clear();
    for (size_t i = 1; i < fn_ast.blocks.size(); ++i) {
        const auto& blk_ast = fn_ast.blocks[i];
        BasicBlock* bb = block_map[blk_ast.id];
        for (const auto& p : blk_ast.params) {
            if (p.second == BronzeType::Bool) {
                Value* param = b.add_block_param(bb, Type::i32());
                zero_one_params_.insert(param);
                val_map[p.first] = param;
            } else {
                val_map[p.first] = b.add_block_param(bb, lower_abi_type(p.second));
            }
        }
    }
    add_handler_params(fn_ast, b, block_map);

    // 4. Lower instructions block by block, in an order where a definition
    // is lowered before its uses (see lowering_order).
    // When arms are not marked for harvesting, precompute value use counts and
    // false constants so a trailing `unbox.bool` -> `[!]` -> `branch` fuses
    // directly into branches without materializing an intermediate 0/1 value.
    std::unordered_map<uint32_t, uint32_t> use_count;
    std::unordered_set<uint32_t> false_const_ids;
    const bool fuse_truthy_branches = !prop_lowering_.arms_marked();
    if (fuse_truthy_branches) {
        auto count_uses = [&](const BronzeBlockTarget& tgt) {
            for (uint32_t id : tgt.args) ++use_count[id];
        };
        for (const auto& blk : fn_ast.blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == BronzeOp::ConstBool && !inst.imm_bool && inst.result_id != UINT32_MAX) {
                    false_const_ids.insert(inst.result_id);
                }
                for (uint32_t id : inst.operands) ++use_count[id];
                if (inst.op == BronzeOp::Jump) {
                    count_uses(inst.target);
                } else if (inst.op == BronzeOp::Branch) {
                    count_uses(inst.target);
                    count_uses(inst.else_target);
                }
            }
        }
    }

    boolean_values_.clear();
    std::vector<uint8_t> loop_headers;
    for (const BronzeBlock* blk_ptr : lowering_order(fn_ast, loop_headers)) {
        const BronzeBlock& blk_ast = *blk_ptr;
        BasicBlock* bb = block_map[blk_ast.id];
        // The IL's own cold mark (a profiled region's slow copy): brass sinks
        // it, and the blocks only it enters go with it.
        if (blk_ast.cold) bb->set_cold();
        b.position_at_end(bb);
        // The loop count of a build made to be harvested, at each header.
        if (fn_ast.entry_slow_site != UINT32_MAX && loop_headers[blk_ptr - fn_ast.blocks.data()] != 0) {
            prop_lowering_.count_index(b, fn_ast.entry_slow_site + kLoopCountOffset);
        }
        uint32_t cont_counter = 0;
        const size_t first_new_block = fn->blocks().size();

        const auto& insts = blk_ast.instructions;
        const size_t n_insts = insts.size();
        size_t fused_unbox_idx = n_insts;
        size_t fused_neg_idx = n_insts;
        size_t fused_br_idx = n_insts;
        bool fused_swap = false;
        if (fuse_truthy_branches && n_insts >= 2 && insts.back().op == BronzeOp::Branch &&
            !insts.back().operands.empty()) {
            const BronzeInstruction& br = insts.back();
            if (block_map.count(br.target.block_id) && block_map.count(br.else_target.block_id)) {
                auto is_dynamic_unbox_bool = [&](const BronzeInstruction& inst) {
                    return inst.op == BronzeOp::Unbox && inst.result_type == BronzeType::Bool &&
                           inst.result_id != UINT32_MAX && !inst.operands.empty() &&
                           use_count[inst.result_id] == 1;
                };
                const uint32_t br_cond_id = br.operands[0];
                const BronzeInstruction& prev = insts[n_insts - 2];
                if (is_dynamic_unbox_bool(prev) && prev.result_id == br_cond_id) {
                    fused_unbox_idx = n_insts - 2;
                    fused_br_idx = n_insts - 1;
                } else if (n_insts >= 3 && prev.op == BronzeOp::CmpEq && prev.result_id != UINT32_MAX &&
                           prev.result_id == br_cond_id && use_count[prev.result_id] == 1 &&
                           prev.operands.size() == 2) {
                    uint32_t unbox_cand_id = UINT32_MAX;
                    if (false_const_ids.count(prev.operands[1])) unbox_cand_id = prev.operands[0];
                    else if (false_const_ids.count(prev.operands[0])) unbox_cand_id = prev.operands[1];
                    if (unbox_cand_id != UINT32_MAX) {
                        size_t cand_idx = n_insts - 3;
                        if (n_insts >= 4 && insts[cand_idx].op == BronzeOp::ConstBool &&
                            insts[cand_idx].result_id != unbox_cand_id) {
                            cand_idx = n_insts - 4;
                        }
                        if (is_dynamic_unbox_bool(insts[cand_idx]) &&
                            insts[cand_idx].result_id == unbox_cand_id) {
                            fused_unbox_idx = cand_idx;
                            fused_neg_idx = n_insts - 2;
                            fused_br_idx = n_insts - 1;
                            fused_swap = true;
                        }
                    }
                }
            }
        }

        for (size_t idx = 0; idx < n_insts; ++idx) {
            if (idx == fused_unbox_idx || idx == fused_neg_idx || idx == fused_br_idx) continue;
            const auto& inst_ast = insts[idx];
            apply_inst_loc(b, inst_ast);
            if (!lower_instruction(inst_ast, b, fn, val_map, block_map, blk_ast.handler_id, blk_ast.id,
                                   &cont_counter)) {
                return false;
            }
        }
        if (fused_br_idx < n_insts) {
            const BronzeInstruction& u = insts[fused_unbox_idx];
            const BronzeInstruction& br = insts[fused_br_idx];
            Value* raw_val = get_val_by_id(u.operands[0], b, val_map);
            if (!raw_val) return false;
            apply_inst_loc(b, u);
            set_slow_site_of(u);
            in_protected_block_ = handler_exception(blk_ast.handler_id) != nullptr;
            BasicBlock* true_bb = block_map.at(br.target.block_id);
            BasicBlock* false_bb = block_map.at(br.else_target.block_id);
            auto get_args = [&](const BronzeBlockTarget& tgt, BasicBlock* target_bb) {
                std::vector<Value*> args;
                for (size_t i = 0; i < tgt.args.size(); ++i) {
                    Value* a = get_val_by_id(tgt.args[i], b, val_map);
                    if (i < target_bb->params().size() && a) a = edge_arg(a, target_bb->params()[i], b);
                    args.push_back(a);
                }
                return args;
            };
            BasicBlock* on_true = fused_swap ? false_bb : true_bb;
            BasicBlock* on_false = fused_swap ? true_bb : false_bb;
            std::vector<Value*> true_args = fused_swap ? get_args(br.else_target, false_bb)
                                                       : get_args(br.target, true_bb);
            std::vector<Value*> false_args = fused_swap ? get_args(br.target, true_bb)
                                                        : get_args(br.else_target, false_bb);
            lower_dynamic_truthy_branch(b, ensure_type(raw_val, Type::i64(), b),
                                        on_true, true_args, on_false, false_args, &prop_lowering_);
        }
        note_protected_blocks(fn, bb, first_new_block, blk_ast.handler_id);
        if (blk_ast.cold) {
            for (size_t bi = first_new_block; bi < fn->blocks().size(); ++bi) fn->blocks()[bi]->set_cold();
        }
    }
    route_exception_edges(b, block_map);

    if (stack_check_entry_bb_ != nullptr) {
        // A function that calls nothing cannot recurse, and its frame is the
        // one the prologue elides when nothing forces it — so the check goes
        // unless the body made a call. A guard's exit is not one: it enters
        // G's twin once, and every call the twin makes reaches a function
        // that checks, so at most this frame and the twin's run unchecked
        // below the limit, inside the 128 KiB the limit reserves (tls_block.cpp).
        bool has_call = false;
        for (BasicBlock* bb : fn->blocks()) {
            if (bb == stack_check_overflow_bb_) continue;
            for (Instruction* inst = bb->head(); inst && !has_call; inst = inst->next()) {
                has_call = inst->is_call();
            }
            if (has_call) break;
        }
        if (!has_call) {
            BasicBlock* entry_bb = stack_check_entry_bb_;
            while (entry_bb->head()) entry_bb->remove_instruction(entry_bb->head());
            b.position_at_end(entry_bb);
            b.build_br(stack_check_body_bb_);
            fn->remove_block(stack_check_overflow_bb_);
        }
        stack_check_entry_bb_ = nullptr;
        stack_check_overflow_bb_ = nullptr;
        stack_check_body_bb_ = nullptr;
    }

    fn->rebuild_cfg_predecessors();
    fn->sort_blocks_rpo();
    fn->rebuild_cfg_predecessors();
    return true;
}

} // namespace il2mir
