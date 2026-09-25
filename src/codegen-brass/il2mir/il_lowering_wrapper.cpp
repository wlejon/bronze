#include "il_lowering.h"
#include "il_abi.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace il2mir {

bool IlLowering::emit_wrapper(const BronzeFunction& fn_ast, Module& mod, const std::string& fn_name, uint32_t declared_param_count, bool is_closure) {
    Function* wfn = mod.get_function("__wrapper_" + fn_name);
    if (!wfn) return false;

    Builder b(mod);
    b.set_function(wfn);

    BasicBlock* bb = b.append_block("entry");
    Value* raw_env = b.add_block_param(bb, Type::i64());
    Value* raw_this = b.add_block_param(bb, Type::i64());
    Value* val_argc = b.add_block_param(bb, Type::i32());
    Value* val_argv = b.add_block_param(bb, Type::ptr());
    b.position_at_end(bb);
    // The wrapper is entered from the runtime with raw bits; each Value is
    // held tagged from the moment it is read, so every helper call below may
    // collect. (argv is the caller's, rooted there.)
    Value* val_env = ensure_type(raw_env, Type::tagged(), b);
    Value* val_this = ensure_type(raw_this, Type::tagged(), b);

    bool needs_env = false;
    bool needs_this = false;
    bool needs_arguments = false;
    bool has_rest = false;
    bool is_strict = false;
    size_t first_source_param = 0;

    auto it_meta = options_.function_meta.find(fn_ast.name);
    if (it_meta == options_.function_meta.end()) {
        it_meta = options_.function_meta.find(fn_name);
    }
    if (it_meta != options_.function_meta.end()) {
        needs_env = it_meta->second.needs_env;
        needs_this = it_meta->second.needs_this;
        needs_arguments = it_meta->second.needs_arguments;
        has_rest = it_meta->second.has_rest_param;
        is_strict = it_meta->second.is_strict;
        first_source_param = it_meta->second.first_source_param;
    } else {
        bool has_module_env_get = false;
        bool uses_p0_as_super_this = false;
        bool uses_p0_as_env = false;
        uint32_t param0_id = fn_ast.params.empty() ? UINT32_MAX : fn_ast.params[0].first;
        for (const auto& blk : fn_ast.blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == BronzeOp::ModuleEnvGet) has_module_env_get = true;
                if (inst.op == BronzeOp::SuperCall && inst.operands.size() >= 2 && inst.operands[1] == param0_id) {
                    uses_p0_as_super_this = true;
                }
                if (inst.op == BronzeOp::EnvGet || inst.op == BronzeOp::EnvSet ||
                    inst.op == BronzeOp::EnvCreate || inst.op == BronzeOp::CreateFunc) {
                    if (!inst.operands.empty() && inst.operands[0] == param0_id) {
                        uses_p0_as_env = true;
                    }
                }
            }
        }
        if (has_module_env_get || uses_p0_as_super_this) {
            needs_env = false;
            needs_this = true;
        } else if (uses_p0_as_env) {
            needs_env = true;
            needs_this = (fn_ast.params.size() > declared_param_count + 1);
        } else {
            bool param0_is_this = false;
            for (const auto& blk : fn_ast.blocks) {
                for (const auto& inst : blk.instructions) {
                    if (inst.op == BronzeOp::PropGet || inst.op == BronzeOp::PropSet ||
                        inst.op == BronzeOp::MethodCall || inst.op == BronzeOp::ElemGet ||
                        inst.op == BronzeOp::ElemSet) {
                        if (!inst.operands.empty() && inst.operands[0] == param0_id) {
                            param0_is_this = true;
                            break;
                        }
                    } else if (inst.op == BronzeOp::Ret) {
                        if (!inst.operands.empty() && inst.operands[0] == param0_id) {
                            param0_is_this = true;
                            break;
                        }
                    }
                }
                if (param0_is_this) break;
            }
            if (param0_is_this) {
                needs_env = false;
                needs_this = true;
            } else {
                needs_env = is_closure;
                needs_this = false;
            }
        }
        first_source_param = (needs_env ? 1 : 0) + (needs_this ? 1 : 0);
    }

    const size_t named_count = (fn_ast.params.size() > first_source_param + (has_rest ? 1 : 0))
        ? (fn_ast.params.size() - first_source_param - (has_rest ? 1 : 0))
        : 0;

    std::vector<Value*> loaded;
    for (size_t n = 0; n < named_count; ++n) {
        Value* idx = b.build_iconst_i32(static_cast<int32_t>(n));
        Value* raw = b.build_call("bronze_arg_at", Type::i64(), {val_argc, val_argv, idx});
        loaded.push_back(ensure_type(raw, Type::tagged(), b));
    }

    Value* arguments_arg = nullptr;
    if (needs_arguments) {
        Value* callee_val = is_strict ? b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag))
                                      : ensure_type(val_env, Type::i64(), b);
        Value* is_strict_val = b.build_iconst_i32(is_strict ? 1 : 0);
        arguments_arg = ensure_type(
            b.build_call("bronze_arguments_object", Type::i64(), {val_argc, val_argv, callee_val, is_strict_val}),
            Type::tagged(), b);
    }

    Value* rest_arg = nullptr;
    if (has_rest) {
        uint32_t first_rest = static_cast<uint32_t>(fn_ast.params.size() - 1 - first_source_param);
        rest_arg = ensure_type(
            b.build_call("bronze_rest_args", Type::i64(),
                         {val_argc, val_argv, b.build_iconst_i32(static_cast<int32_t>(first_rest))}),
            Type::tagged(), b);
    }

    Function* target = mod.get_function(fn_name);
    if (!target) return false;
    const std::vector<Type>& target_params = target->param_types();
    std::vector<Value*> call_args;
    auto push_arg = [&](Value* v) {
        const size_t i = call_args.size();
        call_args.push_back(i < target_params.size() ? ensure_type(v, target_params[i], b) : v);
    };
    if (needs_env) push_arg(val_env);
    if (needs_this) push_arg(val_this);
    if (needs_arguments) push_arg(arguments_arg);

    for (size_t p = first_source_param; p < fn_ast.params.size(); ++p) {
        size_t source_idx = p - first_source_param;
        if (has_rest && p + 1 == fn_ast.params.size()) {
            push_arg(rest_arg);
            break;
        }
        BronzeType param_type = fn_ast.params[p].second;
        if (param_type != BronzeType::F64 && param_type != BronzeType::I32 && param_type != BronzeType::Bool) {
            push_arg(loaded[source_idx]);
            continue;
        }
        // The bits, read right before the helper that converts them.
        Value* raw_i64 = ensure_type(loaded[source_idx], Type::i64(), b);
        if (param_type == BronzeType::F64) {
            bool is_pinned = false;
            uint32_t pin_key = 0;
            if (it_meta != options_.function_meta.end() && p < it_meta->second.params_pinned.size() && it_meta->second.params_pinned[p]) {
                is_pinned = true;
                pin_key = (p < it_meta->second.param_pin_keys.size()) ? it_meta->second.param_pin_keys[p] : 0;
            }
            if (is_pinned) {
                BasicBlock* cur_bb = b.current_block();
                BasicBlock* bad_bb = b.append_block("w_pin_bad_" + std::to_string(p));
                BasicBlock* ok_bb = b.append_block("w_pin_ok_" + std::to_string(p));
                b.position_at_end(cur_bb);
                Value* is_num = b.build_ule(raw_i64, b.build_iconst_i64(static_cast<int64_t>(0xFFF0000000000000ULL)));
                b.build_br_if(is_num, ok_bb, bad_bb);

                b.position_at_end(bad_bb);
                b.build_call("bronze_pin_violation", Type::i64(), {get_key_id(b, pin_key), raw_i64});
                b.build_ret(b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag)));

                b.position_at_end(ok_bb);
                push_arg(b.build_bitcast_f64_i64(raw_i64));
            } else {
                push_arg(b.build_call("bronze_unbox_f64", Type::f64(), {raw_i64}));
            }
        } else if (param_type == BronzeType::I32) {
            push_arg(b.build_call("bronze_unbox_i32", Type::i32(), {raw_i64}));
        } else {
            push_arg(b.build_trunc_i8(b.build_call("bronze_unbox_bool", Type::i32(), {raw_i64})));
        }
    }

    Value* call_res = b.build_call(fn_name, target->return_type(), call_args);

    Value* ret_val = nullptr;
    if (fn_ast.return_type == BronzeType::Void) {
        ret_val = b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag));
    } else if (fn_ast.return_type == BronzeType::F64) {
        ret_val = b.build_call("bronze_box_f64", Type::i64(), {call_res});
    } else if (fn_ast.return_type == BronzeType::I32) {
        ret_val = b.build_call("bronze_box_i32", Type::i64(), {call_res});
    } else if (fn_ast.return_type == BronzeType::Bool) {
        ret_val = b.build_call("bronze_box_bool", Type::i64(), {ensure_type(call_res, Type::i32(), b)});
    } else {
        ret_val = ensure_type(call_res, Type::i64(), b);
    }
    b.build_ret(ret_val);
    wfn->rebuild_cfg_predecessors();
    return true;
}

} // namespace il2mir
