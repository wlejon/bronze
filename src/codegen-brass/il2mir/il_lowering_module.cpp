// Lowering a whole module: every function's body (lower_function, il_lowering.cpp),
// then the module-wide passes, resume twins and data symbols.

#include "il_lowering.h"
#include "il_call_entry.h"
#include "il_prop_stubs.h"
#include "il_lowering_coro.h"
#include "il_lowering_ops.h"
#include "il_lowering_calls.h"
#include "il_abi.h"
#include "il_bridge_verifier.h"
#include "il_pipeline.h"
#include <brass/core/parallel_for.hpp>
#include <brass/mir/coro_transform.hpp>
#include <brass/mir/printer.hpp>
#include <brass/mir/verifier.hpp>
#include "support/timings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace il2mir {

namespace {
// BRONZE_VERIFY_EACH_PASS=1 turns on TranslatorOptions::verify_after_each_pass
// in any build, to name the pass that broke a module from a release binary.
bool verifyEachPassEnv() {
    static const bool on = [] {
        const char* v = std::getenv("BRONZE_VERIFY_EACH_PASS");
        return v && v[0] == '1';
    }();
    return on;
}

// BRONZE_MIR_DUMP=<substr>[,...]: the MIR of every function whose name
// contains one of them, as lowered ("before") and as the pass pipeline left
// it ("after"), on stderr. A diagnostic, the backend's counterpart of
// BRONZE_FINAL_IL_DUMP.
void dumpMir(const brass::Module& mod, const char* when) {
    static const char* const want = std::getenv("BRONZE_MIR_DUMP");
    if (want == nullptr || *want == 0) return;
    const std::string_view all = want;
    for (const brass::Function* fn : mod.functions()) {
        if (fn == nullptr) continue;
        bool hit = false;
        for (size_t at = 0; at <= all.size() && !hit;) {
            const size_t comma = std::min(all.find(',', at), all.size());
            const std::string_view part = all.substr(at, comma - at);
            hit = !part.empty() && fn->name().find(part) != std::string_view::npos;
            at = comma + 1;
        }
        if (hit) std::fprintf(stderr, "; MIR %s the passes\n%s\n", when, brass::to_string(*fn).c_str());
    }
}
}  // namespace

static bool instructions_are_identical(const BronzeInstruction& a, const BronzeInstruction& b) {
    if (a.op != b.op || a.result_type != b.result_type || a.result_id != b.result_id) return false;
    if (a.operands != b.operands) return false;
    if (std::memcmp(&a.imm_f64, &b.imm_f64, sizeof(double)) != 0) return false;
    if (a.imm_i64 != b.imm_i64 || a.imm_bool != b.imm_bool) return false;
    if (a.box_type != b.box_type) return false;
    if (a.callee_name != b.callee_name || a.string_literal != b.string_literal) return false;
    if (a.depth != b.depth || a.index != b.index || a.param_count != b.param_count) return false;
    if (a.ic_index != b.ic_index) return false;
    if (a.target.block_id != b.target.block_id || a.target.args != b.target.args) return false;
    if (a.else_target.block_id != b.else_target.block_id || a.else_target.args != b.else_target.args) return false;
    return true;
}

static bool functions_are_identical(const BronzeFunction& a, const BronzeFunction& b) {
    if (a.params != b.params ||
        a.return_type != b.return_type || a.is_exported != b.is_exported) return false;
    if (a.blocks.size() != b.blocks.size()) return false;
    for (size_t i = 0; i < a.blocks.size(); ++i) {
        const auto& ba = a.blocks[i];
        const auto& bb = b.blocks[i];
        if (ba.id != bb.id || ba.params != bb.params || ba.handler_id != bb.handler_id || ba.cold != bb.cold) return false;
        if (ba.instructions.size() != bb.instructions.size()) return false;
        for (size_t j = 0; j < ba.instructions.size(); ++j) {
            if (!instructions_are_identical(ba.instructions[j], bb.instructions[j])) return false;
        }
    }
    return true;
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
// (runtime_symbols.hpp), so no engine binds it to a call stub. A function
// the module defines under the prefix (the sort merge stub, whose address
// the runtime is handed) is code, not a table.
static void declare_data_symbols(Module& mod, size_t threads) {
    // Each function scanned on its own (the scan only reads the module), the
    // symbols declared in module order.
    const std::vector<Function*>& fns = mod.functions();
    std::vector<std::vector<std::string_view>> per(fns.size());
    std::vector<size_t> weights(fns.size(), 1);
    size_t total = 0;
    for (size_t i = 0; i < fns.size(); ++i) {
        if (fns[i]) weights[i] += fns[i]->block_count();
        total += weights[i];
    }
    brass::parallel_for_each(brass::heaviest_first(weights), brass::threads_for_work(threads, fns.size(), total, 4000),
                             [&](size_t i) {
        const Function* fn = fns[i];
        if (!fn) return;
        for (const BasicBlock* bb : fn->blocks()) {
            if (!bb) continue;
            for (const Instruction* inst : *bb) {
                if (!inst || inst->opcode() != Opcode::func_addr) continue;
                const std::string_view sym = inst->symbol();
                if (is_translator_data_symbol(sym) && !mod.string_symbol(sym) && !mod.get_function(sym))
                    per[i].push_back(sym);
            }
        }
    });
    for (const auto& syms : per) {
        for (std::string_view sym : syms) mod.add_symbol_role(sym, SymbolRole::Data);
    }
}

std::unique_ptr<Module> IlLowering::lower_module(const BronzeModuleAST& ast) {
    bronze::support::PhaseTimer ltimer(bronze::support::timingsEnabled(), 8);
    auto mod = std::make_unique<Module>(ast.name);
    mod->set_allow_fp_reassociation(options_.allow_fp_reassociation);
    mod->set_pinned_tls_register(options_.pin_tls_register);
    // A JS frame's roots are found through its stack maps. brass's collector
    // also reads the stacks conservatively and pins what a word names, except
    // a JS frame's spill slots and locals, which its map describes; so a dead
    // callee-saved register that still names an object keeps it for that
    // collection, and bronze takes that over the allocator's zeroing of the
    // dead registers and slots at every call (brass
    // Module::precise_stack_roots).
    mod->set_precise_stack_roots(true);
    // Every loop backedge compares the stack pointer with the block's
    // stack_limit, the word every prologue checks too, so arming one
    // interrupt (runtime/tls_block.h) stops a runaway loop as well as the
    // next call: a compartment's time slice and its termination.
    if (options_.pin_tls_register) mod->set_interrupt_poll(kBronzeTlsStackLimitOff, "bronze_interrupt_poll");
    // The thread's young range (bronze_abi_tls.h young_base/young_span):
    // brass lowers each write_barrier to a test of its target against it and
    // calls brass_gc_write_barrier only for a target outside it.
    if (options_.pin_tls_register) mod->set_young_range(BRONZE_TLS_YOUNG_BASE_OFF, BRONZE_TLS_YOUNG_SPAN_OFF);
    // The module's tagged allocator: bronze_gc_alloc, Object-tagged, with
    // the allocation window brass bumps inline through the pinned register.
    mod->set_tagged_allocator("bronze_gc_alloc", BRONZE_TLS_ALLOC_CURSOR_OFF, BRONZE_TLS_ALLOC_LIMIT_OFF,
                              static_cast<uint64_t>(BRONZE_ABI_TAG_OBJECT) << BRONZE_ABI_VALUE_TAG_SHIFT);
    // The constants the runtime keeps below the block (bronze_abi_tls.h,
    // BRONZE_TLS_CONSTANTS), read there instead of from each function's pool.
    if (options_.pin_tls_register) {
        int32_t i = 0;
#define BRONZE_PIN_CONSTANT(v) mod->add_pinned_constant(static_cast<uint64_t>(v), BRONZE_TLS_CONSTANT_OFF(i++));
        BRONZE_TLS_CONSTANTS(BRONZE_PIN_CONSTANT)
#undef BRONZE_PIN_CONSTANT
    }
    current_file_id_ = mod->debug_context().get_or_add_file(ast.name.empty() ? "<anonymous>" : ast.name);
    source_file_ids_.clear();
    source_file_ids_.reserve(ast.source_files.size());
    for (const std::string& file : ast.source_files) {
        source_file_ids_.push_back(mod->debug_context().get_or_add_file(file));
    }

    // Register external runtime helper functions
    register_all_module_external_symbols(mod.get(), options_.entry_symbol);
    prop_lowering_.set_key_map_sym(module_sym("__bronze_key_map"));
    prop_lowering_.set_module_sym_fn([this](const char* base) { return module_sym(base); });
    prop_lowering_.set_ic_table(module_sym("__bronze_ic_table"), options_.ic_site_count);
    prop_lowering_.set_elem_cells(&options_.elem_cell_sites,
                                  [this](Builder& b) { return module_data_addr(b, std::string(kElemCellsSymbol)); });
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
        Function* fn = mod->create_function(fn_name, ret_type, Span<const Type>(param_types.data(), param_types.size()));
        fn->set_allow_fp_reassociation(options_.allow_fp_reassociation);

        if (fn_name != "main" && !fn_ast.is_coroutine_body()) {
            std::vector<Type> wrapper_param_types = {Type::i64(), Type::i64(), Type::i32(), Type::ptr()};
            Function* wfn = mod->create_function(
                "__wrapper_" + fn_name,
                Type::i64(),
                Span<const Type>(wrapper_param_types.data(), wrapper_param_types.size()));
            wfn->set_allow_fp_reassociation(options_.allow_fp_reassociation);
        }
    }

    // The module's stubs: the trampoline protected blocks enter code through
    // (il_call_entry.h) and the property stubs (il_prop_stubs.h).
    {
        const Type tramp_params[] = {Type::ptr(), Type::i64(), Type::i64(), Type::i32(), Type::ptr()};
        build_enter_code_trampoline(
            mod->create_function(module_sym(kEnterCodeTrampoline), Type::i64(), Span<const Type>(tramp_params, 5)));
        const Type get_params[] = {Type::i64(), Type::i32(), Type::ptr()};
        build_prop_get_stub(mod->create_function(module_sym(kPropGetStub), Type::i64(), Span<const Type>(get_params, 3)));
        const Type set_params[] = {Type::i64(), Type::i32(), Type::i64(), Type::ptr(), Type::i32()};
        build_prop_set_stub(
            mod->create_function(module_sym(kPropSetStub), Type::void_type(), Span<const Type>(set_params, 5)),
            module_sym(kSetterCallStub));
        const Type setter_params[] = {Type::i64(), Type::i64(), Type::i64()};
        build_setter_call_stub(
            mod->create_function(module_sym(kSetterCallStub), Type::void_type(), Span<const Type>(setter_params, 3)));
        const Type view_get_params[] = {Type::i64(), Type::i32(), Type::i64()};
        build_view_get_stub(
            mod->create_function(module_sym(kViewGetStub), Type::i64(), Span<const Type>(view_get_params, 3)));
        const Type view_set_params[] = {Type::i64(), Type::i32(), Type::i64(), Type::i64(), Type::i32()};
        build_view_set_stub(
            mod->create_function(module_sym(kViewSetStub), Type::void_type(), Span<const Type>(view_set_params, 5)));
        const Type cell_params[] = {Type::i64(), Type::i64(), Type::i64()};
        build_elem_cell_stub(
            mod->create_function(module_sym(kElemCellStub), Type::i64(), Span<const Type>(cell_params, 3)));
        build_elem_get_stub(
            mod->create_function(module_sym(kElemGetStub), Type::i64(), Span<const Type>(cell_params, 2)));
        const Type method_params[] = {Type::i64(), Type::i32(), Type::i32(), Type::ptr(), Type::ptr()};
        build_method_call_stub(
            mod->create_function(module_sym(kMethodCallStub), Type::i64(), Span<const Type>(method_params, 5)));
    }
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
    declare_data_symbols(*mod, options_.build_threads);
    // A lazy body is not there for the scan to see, and declaring its data
    // symbols when it is built would change the module's declarations while
    // other threads read them: every table a function other than the entry
    // can address is declared here (the entry is always built).
    if (!lazy_bodies_.empty()) {
        for (const char* base : {"__bronze_key_map", "__bronze_module_env", "__bronze_template_cells",
                                 "__bronze_ic_table", "__bronze_module_slot", "__bronze_shape_cells",
                                 "__bronze_literal_shapes", "__bronze_proto_cells", "__bronze_member_cells",
                                 "__bronze_elem_cells",
                                 "__bronze_deopt_counts"}) {
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
                insts += bb->instruction_count();
            }
        }
        std::fprintf(stderr, "        mir: %zu functions (%zu lazy), %zu blocks, %zu instructions\n", fns,
                     lazy_bodies_.size(), blocks, insts);
    }
    ltimer.mark("data syms");

    // 3. Verify module
    dumpMir(*mod, "lowered, before");
    if (options_.verify_lowered_module && !verify_bridge_module(*mod, diag_, options_.build_threads)) {
        return nullptr;
    }
    ltimer.mark("verify");

    // 4. Split every coroutine body at its suspends before anything
    // optimizes it: until then a suspend is an ordinary instruction to the
    // passes, which would move a frame read or an unboxed reference across
    // it, where a resume re-enters the body and a collection may have run.
    brass::lower_coroutines(*mod, options_.build_threads);
    ltimer.mark("coroutines");

    // 5. Every generic function's resume twin, cut from its body as lowered
    // (il_lowering_resume.cpp), before anything optimizes either.
    if (!build_resume_twins(*mod)) return nullptr;
    ltimer.mark("twins");

    // 6. Optionally optimize module
    if (options_.enable_optimizations) {
        // A hook only when it verifies: with one, brass takes each run of
        // function-local passes a step at a time (all functions through one
        // pass, then the hook) rather than each function through the whole
        // run; the module comes out the same either way.
        PassPipelineHooks hooks;
        if (options_.verify_after_each_pass || verifyEachPassEnv()) {
            hooks.after_pass = [&](std::string_view name) {
                if (!verify_module(*mod, diag_, options_.build_threads)) {
                    std::fprintf(stderr, "[FATAL] Broken after %.*s\n",
                                 static_cast<int>(name.size()), name.data());
                    return false;
                }
                return true;
            };
        }
        dumpMir(*mod, "before");
        if (!run_bronze_optimization_pipeline(*mod, options_, hooks)) {
            return nullptr;
        }
        dumpMir(*mod, "after");
        // What the passes removed (barriers, guards) is checked against its
        // certificate in every build; the full verifier only where it runs.
        if (options_.verify_lowered_module ? !verify_module(*mod, diag_, options_.build_threads)
                                           : !verify_module_certificates(*mod, diag_)) {
            return nullptr;
        }
        ltimer.mark("optimize");
    }

    return mod;
}

} // namespace il2mir
