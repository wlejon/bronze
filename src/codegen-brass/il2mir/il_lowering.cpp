#include "il_lowering.h"
#include "il_lowering_direct.h"
#include "il_lowering_coro.h"
#include "il_lowering_ops.h"
#include "il_lowering_calls.h"
#include "il_abi.h"
#include "il_bridge_verifier.h"
#include "il_pipeline.h"
#include <brass/mir/coro_transform.hpp>
#include <brass/mir/verifier.hpp>
#include "support/timings.h"
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
    Value* entry = b.build_add(deltas, b.build_shl(slot, b.build_iconst_i64(3)));
    return b.build_load(Type::i64(), entry, 0);
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
    // An element read held as a double (il.h `numberBits`), its bits made at the use: nothing
    // collects before its guard chain and past it they are a Number's, never a moved object.
    if (const auto nb = number_bits_ids_.find(id); nb != number_bits_ids_.end() && it->second) {
        Value* bits = b.build_bitcast_i64_f64(it->second);
        if (!nb->second) return bits;
        Value* is_hole = b.build_eq(bits, b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_HOLE_BITS)));
        return b.build_select(is_hole, b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag)), bits);
    }
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

static bool instructions_are_identical(const BronzeInstruction& a, const BronzeInstruction& b) {
    if (a.op != b.op || a.result_type != b.result_type || a.result_id != b.result_id) return false;
    if (a.operands != b.operands) return false;
    if (std::memcmp(&a.imm_f64, &b.imm_f64, sizeof(double)) != 0) return false;
    if (a.imm_i64 != b.imm_i64 || a.imm_bool != b.imm_bool) return false;
    if (a.box_type != b.box_type || a.raw_unbox != b.raw_unbox || a.nullish_unbox != b.nullish_unbox) return false;
    if (a.callee_name != b.callee_name || a.string_literal != b.string_literal) return false;
    if (a.typed_callee_name != b.typed_callee_name) return false;
    if (a.depth != b.depth || a.index != b.index || a.param_count != b.param_count) return false;
    if (a.static_slot != b.static_slot || a.ic_index != b.ic_index || a.is_mono != b.is_mono || a.is_fn_recv != b.is_fn_recv ||
        a.is_family != b.is_family) return false;
    if (a.static_cell != b.static_cell || a.family_lo != b.family_lo || a.family_span != b.family_span) return false;
    if (a.run_cell != b.run_cell || a.run_length != b.run_length || a.run_slot != b.run_slot) return false;
    if (a.target.block_id != b.target.block_id || a.target.args != b.target.args) return false;
    if (a.else_target.block_id != b.else_target.block_id || a.else_target.args != b.else_target.args) return false;
    return true;
}

static bool functions_are_identical(const BronzeFunction& a, const BronzeFunction& b) {
    if (a.params != b.params || a.param_const_ints != b.param_const_ints ||
        a.return_type != b.return_type || a.is_exported != b.is_exported) return false;
    if (a.blocks.size() != b.blocks.size()) return false;
    for (size_t i = 0; i < a.blocks.size(); ++i) {
        const auto& ba = a.blocks[i];
        const auto& bb = b.blocks[i];
        if (ba.id != bb.id || ba.params != bb.params || ba.handler_id != bb.handler_id) return false;
        if (ba.instructions.size() != bb.instructions.size()) return false;
        for (size_t j = 0; j < ba.instructions.size(); ++j) {
            if (!instructions_are_identical(ba.instructions[j], bb.instructions[j])) return false;
        }
    }
    return true;
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
// that edge is as real as a branch's.
static std::vector<const BronzeBlock*> lowering_order(const BronzeFunction& fn_ast) {
    std::vector<const BronzeBlock*> order;
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
    // the native stack.
    std::vector<uint8_t> visited(fn_ast.blocks.size(), 0);
    std::vector<size_t> post;
    post.reserve(fn_ast.blocks.size());
    std::vector<std::pair<size_t, std::vector<size_t>>> stack;
    visited[0] = 1;
    stack.emplace_back(0, successors(fn_ast.blocks[0]));
    while (!stack.empty()) {
        auto& [node, succ] = stack.back();
        if (succ.empty()) {
            post.push_back(node);
            stack.pop_back();
            continue;
        }
        const size_t next = succ.front();
        succ.erase(succ.begin());
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

// The symbols whose address the translator takes as data rather than code:
// the module tables (`__bronze_*`, defined by the host's object writer or
// registered with the JIT) and the key manifest.
static bool is_translator_data_symbol(std::string_view sym) {
    constexpr std::string_view key_constants = "_key_constants";
    return sym.starts_with("__bronze_") ||
           (sym.size() > key_constants.size() && sym.ends_with(key_constants));
}

// Declares every data symbol a func_addr in `mod` names as `data`
// (runtime_symbols.hpp), so no engine binds it to a call stub.
static void declare_data_symbols(Module& mod) {
    std::vector<std::string> syms;
    for (const Function* fn : mod.functions()) {
        if (!fn) continue;
        for (const BasicBlock* bb : fn->blocks()) {
            if (!bb) continue;
            for (const Instruction* inst : *bb) {
                if (!inst || inst->opcode() != Opcode::func_addr) continue;
                const std::string_view sym = inst->symbol();
                if (is_translator_data_symbol(sym) && !mod.string_symbol(sym)) syms.emplace_back(sym);
            }
        }
    }
    for (const std::string& sym : syms) mod.add_symbol_role(sym, SymbolRole::Data);
}

std::unique_ptr<Module> IlLowering::lower_module(const BronzeModuleAST& ast) {
    bronze::support::PhaseTimer ltimer(bronze::support::timingsEnabled(), 8);
    auto mod = std::make_unique<Module>(ast.name);
    const bool mod_det_fp = options_.deterministic_fp || ast.is_deterministic_fp;
    mod->set_deterministic_fp(mod_det_fp);
    mod->set_allow_fp_reassociation(options_.allow_fp_reassociation && !mod_det_fp);
    mod->set_pinned_tls_register(options_.pin_tls_register);
    // Every loop backedge compares the stack pointer with the block's
    // stack_limit, the word every prologue checks too, so arming one
    // interrupt (runtime/tls_block.h) stops a runaway loop as well as the
    // next call: a compartment's time slice and its termination.
    if (options_.pin_tls_register) mod->set_interrupt_poll(kBronzeTlsStackLimitOff, "bronze_interrupt_poll");
    current_file_id_ = mod->debug_context().get_or_add_file(ast.name.empty() ? "<anonymous>" : ast.name);
    source_file_ids_.clear();
    source_file_ids_.reserve(ast.source_files.size());
    for (const std::string& file : ast.source_files) {
        source_file_ids_.push_back(mod->debug_context().get_or_add_file(file));
    }
    if (!record_inline_scopes(ast, *mod)) return nullptr;

    // Register external runtime helper functions
    register_all_module_external_symbols(mod.get(), options_.entry_symbol);
    prop_lowering_.set_key_map_sym(module_sym("__bronze_key_map"));
    prop_lowering_.set_ic_table(module_sym("__bronze_ic_table"), options_.ic_site_count);
    prop_lowering_.set_static_tables(module_sym("__bronze_static_cells"), options_.static_site_count,
                                     module_sym("__bronze_family_base"), options_.class_family_count);
    if (options_.per_thread_module_data) {
        prop_lowering_.set_module_delta_fn([this](Builder& b) { return module_delta(b); });
        mod->add_external_symbol("bronze_module_instance");
        mod->add_external_symbol(module_sym("__bronze_module_slot"));
        mod->add_external_symbol(module_sym("__bronze_instance"));
        mod->add_external_symbol(module_sym("__bronze_instance_end"));
    }

    current_ast_ = &ast;
    ltimer.mark("externals");

    // 1. Forward-declare all functions (uniquifying any duplicate function names from Bronze)
    std::unordered_map<std::string, std::vector<size_t>> name_to_indices;
    for (size_t i = 0; i < ast.functions.size(); ++i) {
        name_to_indices[ast.functions[i].name].push_back(i);
    }

    caller_to_callee_map_.clear();
    external_signatures_.clear();
    resolved_names_.assign(ast.functions.size(), std::string());
    std::vector<std::string>& resolved_names = resolved_names_;
    std::unordered_set<std::string> curry_names;
    for (const auto& [name, indices] : name_to_indices) {
        if (indices.size() == 1) {
            resolved_names[indices[0]] = name;
        } else {
            // Check if all duplicates are identical
            bool all_identical = true;
            for (size_t k = 1; k < indices.size(); ++k) {
                if (!functions_are_identical(ast.functions[indices[0]], ast.functions[indices[k]])) {
                    all_identical = false;
                    break;
                }
            }
            if (all_identical) {
                resolved_names[indices[0]] = name;
                for (size_t k = 1; k < indices.size(); ++k) {
                    resolved_names[indices[k]] = ""; // skip duplicate definition
                }
                continue;
            }

            if (name == "main") {
                size_t entry_idx = indices.back();
                for (size_t idx : indices) {
                    if (ast.functions[idx].return_type == BronzeType::Void && ast.functions[idx].params.empty()) {
                        entry_idx = idx;
                        break;
                    }
                }
                size_t user_k = 0;
                for (size_t idx : indices) {
                    if (idx == entry_idx) {
                        resolved_names[idx] = "main";
                    } else {
                        resolved_names[idx] = "main$" + std::to_string(user_k++);
                    }
                }
                std::string target_callee = (indices[0] == entry_idx && indices.size() > 1) ? resolved_names[indices[1]] : resolved_names[indices[0]];
                for (size_t fn_idx = 0; fn_idx < ast.functions.size(); ++fn_idx) {
                    caller_to_callee_map_[fn_idx]["main"] = target_callee;
                }
                continue;
            }

            // Check if any function in indices has a self-referential create.func (curry pattern)
            size_t non_leaf_idx = SIZE_MAX;
            for (size_t idx : indices) {
                bool has_self_create = false;
                for (const auto& blk : ast.functions[idx].blocks) {
                    for (const auto& inst : blk.instructions) {
                        if (inst.op == BronzeOp::CreateFunc && inst.callee_name == name) {
                            has_self_create = true;
                            break;
                        }
                    }
                    if (has_self_create) break;
                }
                if (has_self_create) {
                    non_leaf_idx = idx;
                    break;
                }
            }

            if (non_leaf_idx != SIZE_MAX) {
                // Curry pattern: non-leaf closure retains name; leaf closure gets $leaf
                curry_names.insert(name);
                for (size_t idx : indices) {
                    if (idx == non_leaf_idx) {
                        resolved_names[idx] = name;
                    } else {
                        resolved_names[idx] = name + "$leaf";
                    }
                }
                for (size_t fn_idx = 0; fn_idx < ast.functions.size(); ++fn_idx) {
                    if (fn_idx == non_leaf_idx) {
                        caller_to_callee_map_[fn_idx][name] = name + "$leaf";
                    } else {
                        caller_to_callee_map_[fn_idx][name] = name;
                    }
                }
            } else {
                for (size_t k = 0; k < indices.size(); ++k) {
                    resolved_names[indices[k]] = name + "$" + std::to_string(k);
                }
            }
        }
    }

    ltimer.mark("resolve names");
    // Map callers to callees for duplicate function names based on CreateFunc instantiation order and lexical scope inheritance
    caller_create_func_targets_.clear();
    std::unordered_map<std::string, size_t> create_func_occurrence;
    std::unordered_map<size_t, size_t> closure_parent_fn;
    for (size_t fn_idx = 0; fn_idx < ast.functions.size(); ++fn_idx) {
        for (const auto& blk : ast.functions[fn_idx].blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == BronzeOp::CreateFunc) {
                    const std::string& cname = inst.callee_name;
                    auto it = name_to_indices.find(cname);
                    if (it != name_to_indices.end() && it->second.size() > 1 && cname != "main" && !curry_names.count(cname)) {
                        size_t k = create_func_occurrence[cname]++;
                        if (k < it->second.size()) {
                            size_t child_fn_idx = it->second[k];
                            const std::string& child_res_name = resolved_names[child_fn_idx];
                            if (!child_res_name.empty()) {
                                caller_create_func_targets_[fn_idx][cname].push_back(child_res_name);
                                caller_to_callee_map_[fn_idx][cname] = child_res_name;
                                closure_parent_fn[child_fn_idx] = fn_idx;
                            }
                        }
                    }
                }
            }
        }
    }

    // Propagate mappings from parent scopes down to child closures
    for (size_t fn_idx = 0; fn_idx < ast.functions.size(); ++fn_idx) {
        auto pit = closure_parent_fn.find(fn_idx);
        if (pit != closure_parent_fn.end()) {
            size_t p = pit->second;
            auto it_p = caller_to_callee_map_.find(p);
            if (it_p != caller_to_callee_map_.end()) {
                for (const auto& [cname, target] : it_p->second) {
                    if (!curry_names.count(cname) && caller_to_callee_map_[fn_idx].find(cname) == caller_to_callee_map_[fn_idx].end()) {
                        caller_to_callee_map_[fn_idx][cname] = target;
                    }
                }
            }
        }
    }

    // Default fallback for any remaining duplicate function references. The
    // duplicated names are collected first: walking every name for every
    // function was functions x names map probes, for the (usually empty)
    // handful of names that are duplicated.
    std::vector<std::pair<const std::string*, const std::string*>> fallback_names;
    for (const auto& [name, indices] : name_to_indices) {
        if (indices.size() > 1 && name != "main" && !curry_names.count(name) && !resolved_names[indices[0]].empty()) {
            fallback_names.emplace_back(&name, &resolved_names[indices[0]]);
        }
    }
    if (!fallback_names.empty()) {
        for (size_t fn_idx = 0; fn_idx < ast.functions.size(); ++fn_idx) {
            auto& callees = caller_to_callee_map_[fn_idx];
            for (const auto& [name, target] : fallback_names) callees.try_emplace(*name, *target);
        }
    }
    ltimer.mark("name maps");

    callee_param_counts_.clear();
    closure_functions_.clear();
    for (const auto& fn : ast.functions) {
        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == BronzeOp::CreateFunc) {
                    callee_param_counts_[inst.callee_name] = inst.param_count;
                    closure_functions_.insert(inst.callee_name);
                }
            }
        }
    }
    ltimer.mark("closure set");

    for (size_t i = 0; i < ast.functions.size(); ++i) {
        if (resolved_names[i].empty()) continue;
        const auto& fn_ast = ast.functions[i];
        const std::string& fn_name = resolved_names[i];

        // A native import (no body) takes and returns raw bits; a compiled
        // function passes its dynamic values tagged.
        const bool external = fn_ast.blocks.empty();
        std::vector<Type> param_types;
        // A coroutine body declares its frame as a leading parameter, which
        // is how it reads the resume mode off the frame header.
        if (fn_ast.is_coroutine_body()) param_types.push_back(Type::gcref());
        for (const auto& p : fn_ast.params) {
            param_types.push_back(external ? lower_type(p.second) : lower_abi_type(p.second));
        }
        Type ret_type = external ? lower_type(fn_ast.return_type) : lower_abi_type(fn_ast.return_type);
        if (external) {
            external_signatures_[fn_name] = {ret_type, std::move(param_types)};
            continue;
        }
        const bool fn_det_fp = mod_det_fp || fn_ast.is_deterministic_fp;
        Function* fn = mod->create_function(fn_name, ret_type, Span<const Type>(param_types.data(), param_types.size()));
        fn->set_deterministic_fp(fn_det_fp);
        fn->set_allow_fp_reassociation(options_.allow_fp_reassociation && !fn_det_fp);

        if (fn_name != "main" && !fn_ast.is_coroutine_body()) {
            std::vector<Type> wrapper_param_types = {Type::i64(), Type::i64(), Type::i32(), Type::ptr()};
            Function* wfn = mod->create_function(
                "__wrapper_" + fn_name,
                Type::i64(),
                Span<const Type>(wrapper_param_types.data(), wrapper_param_types.size()));
            wfn->set_deterministic_fp(fn_det_fp);
            wfn->set_allow_fp_reassociation(options_.allow_fp_reassociation && !fn_det_fp);
        }
    }

    emit_direct_stubs(this, *mod);
    ltimer.mark("declare");
    // 2. Lower each function body, or leave it lazy (il_lowering_lazy.cpp).
    lazy_bodies_.clear();
    for (size_t i = 0; i < ast.functions.size(); ++i) {
        if (resolved_names[i].empty()) continue;
        if (ast.functions[i].blocks.empty()) continue;
        if (options_.lazy_bodies && !must_lower_eagerly(i)) {
            Function* fn = mod->get_function(resolved_names[i]);
            if (!fn) return nullptr;
            fn->mark_lazy();
            lazy_bodies_.emplace(fn, LazyBody{i, false});
            if (Function* wfn = mod->get_function("__wrapper_" + resolved_names[i])) {
                wfn->mark_lazy();
                lazy_bodies_.emplace(wfn, LazyBody{i, true});
            }
            continue;
        }
        // The entry runs once: built in its own arena, it can be released
        // after it has run, as a lazy body is, and the provider builds it
        // again should a deopt or a tier-up of its loops ask.
        if (options_.lazy_bodies && resolved_names_[i] == "main" && entry_rebuildable(i)) {
            Function* fn = mod->get_function(resolved_names_[i]);
            if (!fn) return nullptr;
            fn->own_body_arena();
            LazyBody& entry = lazy_bodies_[fn];
            entry.index = i;
            if (!build_provided_body(*fn, entry)) return nullptr;
            fn->note_built();
            continue;
        }
        if (!lower_body(i, *mod) || !lower_wrapper(i, *mod)) return nullptr;
    }

    ltimer.mark("lower fns");
    declare_data_symbols(*mod);
    // A lazy body is not there for the scan to see, and declaring its data
    // symbols when it is built would change the module's declarations while
    // other threads read them: every table a function other than the entry
    // can address is declared here (the entry is always built).
    if (!lazy_bodies_.empty()) {
        for (const char* base : {"__bronze_key_map", "__bronze_module_env", "__bronze_template_cells",
                                 "__bronze_ic_table", "__bronze_module_slot", "__bronze_static_cells",
                                 "__bronze_family_base"}) {
            mod->add_symbol_role(module_sym(base), SymbolRole::Data);
        }
    }
    if (bronze::support::timingsEnabled()) {
        size_t fns = 0, blocks = 0, insts = 0;
        for (const Function* f : mod->functions()) {
            if (!f) continue;
            ++fns;
            for (const BasicBlock* bb : f->blocks()) {
                ++blocks;
                for (const Instruction* inst : *const_cast<BasicBlock*>(bb)) insts += inst ? 1 : 0;
            }
        }
        std::fprintf(stderr, "        mir: %zu functions (%zu lazy), %zu blocks, %zu instructions\n", fns,
                     lazy_bodies_.size(), blocks, insts);
    }
    ltimer.mark("data syms");

    // 3. Verify module
    if (options_.verify_lowered_module && !verify_bridge_module(*mod, diag_)) {
        return nullptr;
    }
    ltimer.mark("verify");

    // 4. Split every coroutine body at its suspends before anything
    // optimizes it: until then a suspend is an ordinary instruction to the
    // passes, which would move a frame read or an unboxed reference across
    // it, where a resume re-enters the body and a collection may have run.
    brass::lower_coroutines(*mod);
    ltimer.mark("coroutines");

    // 5. Optionally optimize module
    if (options_.enable_optimizations) {
        PassPipelineHooks hooks;
        hooks.after_pass = [&](std::string_view name) {
            if (!options_.verify_after_each_pass) return true;
            if (!verify_module(*mod, diag_)) {
                std::fprintf(stderr, "[FATAL] Broken after %.*s\n",
                             static_cast<int>(name.size()), name.data());
                return false;
            }
            return true;
        };
        if (!run_bronze_optimization_pipeline(*mod, options_, hooks)) {
            return nullptr;
        }
        // What the passes removed (barriers, guards) is checked against its
        // certificate in every build; the full verifier only where it runs.
        if (options_.verify_lowered_module ? !verify_module(*mod, diag_)
                                           : !verify_module_certificates(*mod, diag_)) {
            return nullptr;
        }
        ltimer.mark("optimize");
    }

    return mod;
}

bool IlLowering::lower_function(const BronzeFunction& fn_ast, Module& mod, const std::string& fn_name,
                                Function* target) {
    Function* fn = target ? target : mod.get_function(fn_name);
    if (!fn) return false;

    module_env_regs_.clear();
    current_fn_dynamic_ids_.clear();
    number_bits_ids_.clear();
    create_func_counter_.clear();
    argv_block_ = nullptr;
    argv_block_words_ = 0;
    current_module_delta_ = nullptr;
    begin_hoisting(nullptr);
    analyze_function(fn_ast);

    // The argv block a call stages its arguments in (il_lowering_calls.h).
    const uint32_t widest_method_argc = argv_block_words(this, fn_ast);

    Builder b(mod);
    b.set_function(fn);
    // The lowering spells every tag, offset and limit inline; one of each per
    // block is enough (BRONZE_NO_CONST_REUSE=1 for the A/B).
    static const bool no_const_reuse = std::getenv("BRONZE_NO_CONST_REUSE") != nullptr;
    b.set_const_reuse(!no_const_reuse);
    // Every function carries guards (il_speculation.h), a coroutine body
    // too: brass's coroutine lowering keeps its suspend states out of the
    // guards' resume ids and adds the frame, and anything it reloads, to
    // each guard's state, so a deopt finishes the resume in Tier 0 over the
    // same frame. The argv block is an alloca, which a resume re-creates in
    // the new frame with its contents.
    // The entry's symbol differs between a JIT run and an AOT build; a
    // profile names it kSpecEntryName (il_spec_profile.h).
    const bool is_entry = fn_name == "main" || fn_name == options_.entry_symbol;
    spec_.begin_function(true, is_entry ? std::string(kSpecEntryName) : fn_name);

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
        for (size_t i = 0; i < fn_ast.params.size(); ++i) {
            uint32_t param_id = fn_ast.params[i].first;
            Type param_type = lower_abi_type(fn_ast.params[i].second);
            Value* param_val = b.add_block_param(entry_bb, param_type);
            val_map[param_id] = param_val;
        }

        b.position_at_end(entry_bb);
        // In the entry block itself, ahead of the stack check's split: a
        // coroutine body's allocas must be there to be re-created on every
        // resume (brass's coroutine lowering refuses any other).
        if (widest_method_argc > 0) {
            argv_block_ = b.build_alloca_tagged(widest_method_argc);
            argv_block_words_ = widest_method_argc;
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
            b.build_call("bronze_register_value_cells", Type::void_type(), {tpl_addr, tpl_cells_count});
            const std::string key_sym = (options_.entry_symbol.empty() || options_.entry_symbol == "main" || options_.entry_symbol == "bronze_main")
                                            ? "bronze_main_key_constants"
                                            : (options_.entry_symbol + "_key_constants");
            Value* manifest_addr = b.build_func_addr(key_sym);
            Value* map_addr = b.build_func_addr(module_sym("__bronze_key_map"));
            b.build_call("bronze_register_key_manifest", Type::void_type(), {manifest_addr, map_addr});
            register_class_families(b, map_addr);
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
        // Everything the body shares is computed here, where it dominates
        // every block, and only once something first asks for it.
        if (!fn_ast.is_coroutine_body()) {
            for (size_t i = 0; i < fn_ast.params.size() && i < fn_ast.param_const_ints.size(); ++i) {
                if (fn_ast.param_const_ints[i].has_value() &&
                    fn_ast.params[i].second == BronzeType::F64) {
                    val_map[fn_ast.params[i].first] =
                        b.build_fconst_f64(static_cast<double>(*fn_ast.param_const_ints[i]));
                }
            }
        }
        begin_hoisting(b.current_block());
    }

    // 3. Set up non-entry block parameters
    for (size_t i = 1; i < fn_ast.blocks.size(); ++i) {
        const auto& blk_ast = fn_ast.blocks[i];
        BasicBlock* bb = block_map[blk_ast.id];
        for (const auto& p : blk_ast.params) {
            uint32_t param_id = p.first;
            Type param_type = lower_abi_type(p.second);
            Value* param_val = b.add_block_param(bb, param_type);
            val_map[param_id] = param_val;
        }
    }
    add_handler_params(fn_ast, b, block_map);

    // 4. Lower instructions block by block, in an order where a definition
    // is lowered before its uses (see lowering_order).
    memo_begin_function(fn_ast);
    for (const BronzeBlock* blk_ptr : lowering_order(fn_ast)) {
        const BronzeBlock& blk_ast = *blk_ptr;
        BasicBlock* bb = block_map[blk_ast.id];
        b.position_at_end(bb);
        uint32_t cont_counter = 0;
        const size_t first_new_block = fn->blocks().size();
        run_once_ = run_once_blocks_.count(blk_ast.id) != 0;
        in_loop_ = loop_blocks_.count(blk_ast.id) != 0;
        protected_block_ = blk_ast.handler_id != UINT32_MAX;
        begin_block_proofs(blk_ast.id);
        forget_ta_cursors();
        memo_begin_block(blk_ast);

        for (size_t inst_idx = 0; inst_idx < blk_ast.instructions.size(); ++inst_idx) {
            const MirMark mark = mir_mark(b, *fn);
            // A static run carries the shape memo across its join itself; the
            // other runs' slow copies call helpers, so it is forgotten there.
            const bool other_run =
                try_lower_transition_run(blk_ast, inst_idx, b, fn, val_map, block_map, &cont_counter) ||
                try_lower_array_index_run(blk_ast, inst_idx, b, fn, val_map, block_map, &cont_counter) ||
                try_lower_elem_offset_run(blk_ast, inst_idx, b, fn, val_map, block_map, &cont_counter);
            if (other_run || try_lower_static_slot_run(blk_ast, inst_idx, b, fn, val_map, block_map, &cont_counter)) {
                if (has_error_) return false;
                settle_ta_cursors(mark, *fn);
                if (other_run) memo_kill();
                shape_memo_kept_ = false;
                continue;
            }
            const auto& inst_ast = blk_ast.instructions[inst_idx];
            apply_inst_loc(b, inst_ast);
            if (!lower_instruction(inst_ast, b, fn, val_map, block_map, blk_ast.handler_id, blk_ast.id, &cont_counter)) {
                return false;
            }
            settle_ta_cursors(mark, *fn);
            after_instruction_proofs(inst_ast);
            memo_after_instruction(inst_ast);
        }
        memo_end_block(blk_ast);
        note_protected_blocks(fn, bb, first_new_block, blk_ast.handler_id);
    }
    run_once_ = false;
    in_loop_ = false;
    protected_block_ = false;
    route_exception_edges(b, block_map);

    if (stack_check_entry_bb_ != nullptr) {
        // A function that calls nothing cannot recurse, and its frame is the
        // one the prologue elides when nothing forces it — so the check goes
        // unless the body made a call.
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
    spec_.finish_function(*fn);
    return true;
}

} // namespace il2mir
