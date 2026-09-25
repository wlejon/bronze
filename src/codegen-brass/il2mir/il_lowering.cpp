#include "il_lowering.h"
#include "il_lowering_coro.h"
#include "il_abi.h"
#include "il_pipeline.h"
#include <brass/mir/verifier.hpp>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace il2mir {

Type lower_type(BronzeType t) {
    switch (t) {
        case BronzeType::Void: return Type::void_type();
        case BronzeType::Bool: return Type::i8();
        case BronzeType::I32: return Type::i32();
        case BronzeType::F64: return Type::f64();
        case BronzeType::Str: return Type::ptr();
        case BronzeType::Dynamic: return Type::i64();
        case BronzeType::Unknown: return Type::i64();
    }
    return Type::i64();
}

Type lower_abi_type(BronzeType t) {
    if (t == BronzeType::Dynamic || t == BronzeType::Unknown) return Type::tagged();
    return lower_type(t);
}

namespace {

// Whether `inst` is a point where a collection can run: a call, or an
// explicit safepoint. Between two such points no object moves, so the bits
// of a tagged value are the same wherever in that stretch they are read.
bool may_collect(const Instruction* inst) {
    return inst->is_call() || inst->opcode() == Opcode::safepoint;
}

} // namespace

Value* IlLowering::unbox_tagged(Value* tagged, Builder& b) {
    // box(x) read back before anything could collect is x itself: the bits
    // have not had a chance to move. The scan is bounded; past it the read
    // is an ordinary unbox.
    Instruction* def = tagged->defining_instruction();
    if (def && def->opcode() == Opcode::bitcast_tagged_i64 && def->parent() == b.current_block()) {
        int budget = 32;
        for (Instruction* i = b.current_block()->tail(); i && budget-- > 0; i = i->prev()) {
            if (i == def) return def->operand(0);
            if (may_collect(i)) break;
        }
    }
    return b.build_bitcast_i64_tagged(tagged);
}

IlLowering::IlLowering(const TranslatorOptions& options, DiagnosticReporter* diag)
    : options_(options), diag_(diag), prop_lowering_(options.enable_inlined_fastpaths),
      alloc_lowering_(options.enable_tlab) {}

Value* IlLowering::ensure_type(Value* val, Type target_type, Builder& b) {
    if (!val || val->type() == target_type) return val;

    // A tagged value is the rooted form of a Value's bits: boxing takes the
    // bits as an i64, and box(unbox(x)) is x (x is the one of the two a
    // collection keeps current).
    if (target_type.is_tagged()) {
        Value* bits = ensure_type(val, Type::i64(), b);
        Instruction* def = bits->defining_instruction();
        if (def && def->opcode() == Opcode::bitcast_i64_tagged) return def->operand(0);
        return b.build_bitcast_tagged_i64(bits);
    }
    if (val->type().is_tagged()) {
        return ensure_type(unbox_tagged(val, b), target_type, b);
    }

    Type src_type = val->type();
    if (target_type == Type::i8()) {
        if (src_type == Type::i64()) {
            val = b.build_trunc_i32(val);
        }
        return b.build_trunc_i8(val);
    }
    if (src_type == Type::i8()) {
        val = b.build_zext_i64(val);
        if (target_type == Type::i32()) {
            return b.build_trunc_i32(val);
        }
        src_type = Type::i64();
    }
    if (src_type == Type::f64() && target_type == Type::i32()) {
        return b.build_fptosi_i32(val);
    }
    if (src_type == Type::i32() && target_type == Type::f64()) {
        return b.build_sitofp_f64_i32(val);
    }
    if (src_type == Type::i32() && target_type == Type::i64()) {
        return b.build_sext_i64(val);
    }
    if (src_type == Type::i64() && target_type == Type::i32()) {
        return b.build_trunc_i32(val);
    }
    if (src_type == Type::f64() && target_type == Type::i64()) {
        if (val->defining_instruction() &&
            val->defining_instruction()->opcode() == Opcode::bitcast_f64_i64 &&
            val->defining_instruction()->operand(0)->type() == Type::i64()) {
            return val->defining_instruction()->operand(0);
        }
        Value* bits = b.build_bitcast_i64_f64(val);
        Value* abs_bits = b.build_and(bits, b.build_iconst_i64(static_cast<int64_t>(0x7FFFFFFFFFFFFFFFULL)));
        Value* is_nan = b.build_ugt(abs_bits, b.build_iconst_i64(static_cast<int64_t>(0x7FF0000000000000ULL)));
        return b.build_select(is_nan, b.build_iconst_i64(static_cast<int64_t>(0x7FF8000000000000ULL)), bits);
    }
    if (src_type == Type::i64() && target_type == Type::f64()) {
        return b.build_bitcast_f64_i64(val);
    }
    return val;
}

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
    if (a.box_type != b.box_type || a.raw_unbox != b.raw_unbox) return false;
    if (a.callee_name != b.callee_name || a.string_literal != b.string_literal) return false;
    if (a.depth != b.depth || a.index != b.index || a.param_count != b.param_count) return false;
    if (a.static_slot != b.static_slot || a.ic_index != b.ic_index || a.is_mono != b.is_mono || a.is_fn_recv != b.is_fn_recv) return false;
    if (a.target.block_id != b.target.block_id || a.target.args != b.target.args) return false;
    if (a.else_target.block_id != b.else_target.block_id || a.else_target.args != b.else_target.args) return false;
    return true;
}

static bool functions_are_identical(const BronzeFunction& a, const BronzeFunction& b) {
    if (a.params != b.params || a.return_type != b.return_type || a.is_exported != b.is_exported) return false;
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
    auto mod = std::make_unique<Module>(ast.name);
    mod->set_allow_fp_reassociation(options_.allow_fp_reassociation);
    mod->set_pinned_tls_register(options_.pin_tls_register);
    current_file_id_ = mod->debug_context().get_or_add_file(ast.name.empty() ? "<anonymous>" : ast.name);
    source_file_ids_.clear();
    source_file_ids_.reserve(ast.source_files.size());
    for (const std::string& file : ast.source_files) {
        source_file_ids_.push_back(mod->debug_context().get_or_add_file(file));
    }

    // Register external runtime helper functions
    register_all_module_external_symbols(mod.get(), options_.entry_symbol);
    prop_lowering_.set_key_map_sym(module_sym("__bronze_key_map"));
    prop_lowering_.set_ic_table(module_sym("__bronze_ic_table"), options_.ic_site_count);
    if (options_.per_thread_module_data) {
        prop_lowering_.set_module_delta_fn([this](Builder& b) { return module_delta(b); });
        mod->add_external_symbol("bronze_module_instance");
        mod->add_external_symbol(module_sym("__bronze_module_slot"));
        mod->add_external_symbol(module_sym("__bronze_instance"));
        mod->add_external_symbol(module_sym("__bronze_instance_end"));
    }

    current_ast_ = &ast;




    // 1. Forward-declare all functions (uniquifying any duplicate function names from Bronze)
    std::unordered_map<std::string, std::vector<size_t>> name_to_indices;
    for (size_t i = 0; i < ast.functions.size(); ++i) {
        name_to_indices[ast.functions[i].name].push_back(i);
    }

    caller_to_callee_map_.clear();
    external_signatures_.clear();
    std::vector<std::string> resolved_names(ast.functions.size());
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

    // Default fallback for any remaining duplicate function references
    for (size_t fn_idx = 0; fn_idx < ast.functions.size(); ++fn_idx) {
        for (const auto& [name, indices] : name_to_indices) {
            if (indices.size() > 1 && name != "main" && !curry_names.count(name) && !resolved_names[indices[0]].empty()) {
                if (caller_to_callee_map_[fn_idx].find(name) == caller_to_callee_map_[fn_idx].end()) {
                    caller_to_callee_map_[fn_idx][name] = resolved_names[indices[0]];
                }
            }
        }
    }

    std::unordered_map<std::string, uint32_t> callee_param_counts;
    std::unordered_set<std::string> closure_functions;
    for (const auto& fn : ast.functions) {
        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == BronzeOp::CreateFunc) {
                    callee_param_counts[inst.callee_name] = inst.param_count;
                    closure_functions.insert(inst.callee_name);
                }
            }
        }
    }

    for (size_t i = 0; i < ast.functions.size(); ++i) {
        if (resolved_names[i].empty()) continue;
        const auto& fn_ast = ast.functions[i];
        const std::string& fn_name = resolved_names[i];

        // A native import (no body) takes and returns raw bits; a compiled
        // function passes its dynamic values tagged.
        const bool external = fn_ast.blocks.empty();
        std::vector<Type> param_types;
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

        if (fn_name != "main") {
            std::vector<Type> wrapper_param_types = {Type::i64(), Type::i64(), Type::i32(), Type::ptr()};
            Function* wfn = mod->create_function(
                "__wrapper_" + fn_name,
                Type::i64(),
                Span<const Type>(wrapper_param_types.data(), wrapper_param_types.size()));
            wfn->set_allow_fp_reassociation(options_.allow_fp_reassociation);
        }
    }

    // 2. Lower each function body
    for (size_t i = 0; i < ast.functions.size(); ++i) {
        if (resolved_names[i].empty()) continue;
        const auto& fn_ast = ast.functions[i];
        if (fn_ast.blocks.empty()) continue;
        current_fn_idx_ = i;
        const std::string& fn_name = resolved_names[i];
        if (!lower_function(fn_ast, *mod, fn_name)) {
            return nullptr;
        }
        if (fn_name != "main") {
            uint32_t arity = static_cast<uint32_t>(fn_ast.params.size());
            auto it_ar = callee_param_counts.find(fn_ast.name);
            if (it_ar != callee_param_counts.end()) {
                arity = it_ar->second;
            } else {
                auto it_res = callee_param_counts.find(fn_name);
                if (it_res != callee_param_counts.end()) {
                    arity = it_res->second;
                }
            }
            bool is_closure = closure_functions.count(fn_ast.name) || closure_functions.count(fn_name);
            if (!emit_wrapper(fn_ast, *mod, fn_name, arity, is_closure)) {
                return nullptr;
            }
        }
    }

    declare_data_symbols(*mod);

    // 3. Verify module
    if (!verify_module(*mod, diag_)) {
        return nullptr;
    }

    // 4. Optionally optimize module
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
        if (!run_pass_pipeline(*mod, pass_pipeline_options(options_), hooks)) {
            return nullptr;
        }
        if (!verify_module(*mod, diag_)) {
            return nullptr;
        }
    }

    return mod;
}

bool IlLowering::lower_function(const BronzeFunction& fn_ast, Module& mod, const std::string& fn_name) {
    Function* fn = mod.get_function(fn_name);
    if (!fn) return false;

    module_env_regs_.clear();
    current_fn_dynamic_ids_.clear();
    create_func_counter_.clear();
    argv_block_ = nullptr;
    argv_block_words_ = 0;
    current_module_delta_ = nullptr;

    // Every dynamic value is a tagged SSA value (lower_abi_type): the stack
    // maps of every tier describe it, so bronze's collector finds and
    // updates a live Value whichever engine runs the function.
    auto is_dynamic = [](BronzeType t) { return t == BronzeType::Dynamic || t == BronzeType::Unknown; };
    for (const auto& p : fn_ast.params) {
        if (is_dynamic(p.second)) current_fn_dynamic_ids_.insert(p.first);
    }
    for (const auto& blk : fn_ast.blocks) {
        for (const auto& p : blk.params) {
            if (is_dynamic(p.second)) current_fn_dynamic_ids_.insert(p.first);
        }
        for (const auto& inst : blk.instructions) {
            if (inst.result_id != UINT32_MAX && is_dynamic(inst.result_type)) {
                current_fn_dynamic_ids_.insert(inst.result_id);
            }
        }
    }
    // The argv block a call stages its arguments in when the helper takes
    // `const uint64_t* argv` (a dynamic method call, and any call past the
    // fixed-arity helpers): one alloca.tagged of the widest such call's
    // arguments, so they stay rooted, and are updated, while the helper
    // can collect. Nested calls cannot overlap: an argument is a value
    // already computed by the time its call stores it, and the stores
    // happen right before the call.
    uint32_t widest_method_argc = 0;
    for (const auto& blk : fn_ast.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == BronzeOp::MethodCall && inst.param_count > widest_method_argc) {
                widest_method_argc = inst.param_count;
            }
            if ((inst.op == BronzeOp::Construct || inst.op == BronzeOp::SuperCall || inst.op == BronzeOp::CallDynamic) &&
                inst.param_count > 16 && inst.param_count > widest_method_argc) {
                widest_method_argc = inst.param_count;
            }
        }
    }

    Builder b(mod);
    b.set_function(fn);

    std::unordered_map<uint32_t, BasicBlock*> block_map;
    std::unordered_map<uint32_t, Value*> val_map;

    // 1. Create all basic blocks in advance
    for (const auto& blk_ast : fn_ast.blocks) {
        std::string bb_name = "b" + std::to_string(blk_ast.id);
        BasicBlock* bb = b.append_block(bb_name);
        block_map[blk_ast.id] = bb;
    }

    // 2. Set up entry block parameters from function arguments
    if (!fn_ast.blocks.empty() && block_map.count(fn_ast.blocks[0].id)) {
        BasicBlock* entry_bb = block_map[fn_ast.blocks[0].id];
        for (size_t i = 0; i < fn_ast.params.size(); ++i) {
            uint32_t param_id = fn_ast.params[i].first;
            Type param_type = lower_abi_type(fn_ast.params[i].second);
            Value* param_val = b.add_block_param(entry_bb, param_type);
            val_map[param_id] = param_val;
        }

        b.position_at_end(entry_bb);
        if (options_.pin_tls_register) {
            // The module entry is the one function the runtime calls without
            // its trampoline (bro and the CLI call the exported symbol
            // directly), so it fetches the block itself. Every other
            // function arrives with the register set by its caller: a
            // compiled caller never touches it, a runtime caller went
            // through `bronze_enter_js`.
            if (fn_name == "main") {
                Value* tls = b.build_call("bronze_tls_enter", Type::i64(), {});
                b.build_pinned_tls_write(tls);
            }
            // Stack-limit check, before anything is pushed: below the limit,
            // the runtime raises the RangeError, which leaves this frame as
            // any throw from a call does.
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
            b.build_unreachable();
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
        if (widest_method_argc > 0) {
            argv_block_ = b.build_alloca_tagged(widest_method_argc);
            argv_block_words_ = widest_method_argc;
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
    for (const BronzeBlock* blk_ptr : lowering_order(fn_ast)) {
        const BronzeBlock& blk_ast = *blk_ptr;
        BasicBlock* bb = block_map[blk_ast.id];
        b.position_at_end(bb);
        uint32_t cont_counter = 0;
        const size_t first_new_block = fn->blocks().size();

        for (const auto& inst_ast : blk_ast.instructions) {
            if (current_file_id_ != 0 && inst_ast.line > 0) {
                // The instruction's own file: a merged top level holds lines
                // of every imported module, so the module's name is only the
                // fallback for an instruction that names none.
                const uint32_t fileId = inst_ast.file < source_file_ids_.size()
                                            ? source_file_ids_[inst_ast.file]
                                            : current_file_id_;
                b.set_current_loc(DebugLoc(fileId, inst_ast.line, inst_ast.column));
            } else {
                b.clear_current_loc();
            }
            if (!lower_instruction(inst_ast, b, fn, val_map, block_map, blk_ast.handler_id, blk_ast.id, &cont_counter)) {
                return false;
            }
        }
        note_protected_blocks(fn, bb, first_new_block, blk_ast.handler_id);
    }
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
    return true;
}

} // namespace il2mir
