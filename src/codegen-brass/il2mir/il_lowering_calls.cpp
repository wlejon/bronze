#include "il_lowering_calls.h"
#include "il_lowering.h"
#include "il_abi.h"
#include <brass/mir/module.hpp>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace il2mir {

bool is_call_il_op(BronzeOp op) {
    switch (op) {
        case BronzeOp::Construct:
        case BronzeOp::ConstructSpread:
        case BronzeOp::MethodCall:
        case BronzeOp::MethodCallSpread:
        case BronzeOp::SuperCall:
        case BronzeOp::SuperCallSpread:
        case BronzeOp::SuperGet:
        case BronzeOp::SuperSet:
        case BronzeOp::CallDynamic:
        case BronzeOp::DynamicCallSpread:
        case BronzeOp::FuncRef:
        case BronzeOp::CreateFunc:
            return true;
        default:
            return false;
    }
}

Value* build_call_dynamic(Builder& b, const std::vector<Value*>& dyn_args, IlLowering* lowering) {
    size_t argc = dyn_args.size() > 2 ? dyn_args.size() - 2 : 0;
    if (argc <= 16) {
        std::string helper = "bronze_call_dynamic_" + std::to_string(argc);
        return b.build_call(helper, Type::i64(), Span<Value* const>(dyn_args.data(), dyn_args.size()));
    }
    Value* argv = lowering->stage_argv(b, std::vector<Value*>(dyn_args.begin() + 2, dyn_args.end()));
    Value* argc_val = b.build_iconst_i32(static_cast<int32_t>(argc));
    return b.build_call("bronze_call_dynamic_n", Type::i64(), {dyn_args[0], dyn_args[1], argc_val, argv});
}

// One alloca.tagged of the widest such call's arguments, so they stay rooted,
// and are updated, while the callee or the helper can collect. Nested calls
// cannot overlap: an argument is a value already computed by the time its call
// stores it, and the stores happen right before the call.
uint32_t argv_block_words(const IlLowering* /*lowering*/, const BronzeFunction& fn_ast) {
    uint32_t widest = 0;
    for (const auto& blk : fn_ast.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == BronzeOp::MethodCall && inst.param_count > widest) widest = inst.param_count;
            if ((inst.op == BronzeOp::Construct || inst.op == BronzeOp::SuperCall || inst.op == BronzeOp::CallDynamic) &&
                inst.param_count > 16 && inst.param_count > widest) {
                widest = inst.param_count;
            }
        }
    }
    return widest;
}

bool lower_call_instruction(
    IlLowering* lowering,
    const BronzeInstruction& inst_ast,
    Builder& b,
    Function* /*fn*/,
    std::unordered_map<uint32_t, Value*>& val_map,
    Value*& res_val
) {
    auto get_opd = [&](size_t idx) -> Value* {
        if (idx < inst_ast.operands.size()) {
            uint32_t id = inst_ast.operands[idx];
            if (lowering) {
                return lowering->get_val_by_id(id, b, val_map);
            }
            if (val_map.count(id)) return val_map[id];
        }
        return nullptr;
    };

    auto ensure_type = [&](Value* val, Type target_type) -> Value* {
        return lowering ? lowering->ensure_type(val, target_type, b) : val;
    };

    switch (inst_ast.op) {
        case BronzeOp::Construct: {
            Value* ctor = ensure_type(get_opd(0), Type::i64());
            uint32_t argc = inst_ast.param_count;
            if (argc <= 16) {
                std::vector<Value*> call_args = {ctor};
                for (uint32_t i = 0; i < argc; ++i) {
                    call_args.push_back(ensure_type(get_opd(1 + i), Type::i64()));
                }
                std::string helper = "bronze_construct_" + std::to_string(argc);
                res_val = b.build_call(helper, Type::i64(), call_args);
            } else {
                std::vector<Value*> args;
                for (uint32_t i = 0; i < argc; ++i) args.push_back(get_opd(1 + i));
                Value* argv = lowering->stage_argv(b, args);
                Value* argc_val = b.build_iconst_i32(static_cast<int32_t>(argc));
                res_val = b.build_call("bronze_construct_n", Type::i64(), {ctor, argc_val, argv});
            }
            break;
        }

        case BronzeOp::ConstructSpread: {
            Value* callee = ensure_type(get_opd(0), Type::i64());
            Value* args = ensure_type(get_opd(1), Type::i64());
            res_val = b.build_call("bronze_construct_spread", Type::i64(), {callee, args});
            break;
        }

        // The site's method cache through bronze_call_method, or, with no
        // site, the method read and called.
        case BronzeOp::MethodCall: {
            Value* recv = ensure_type(get_opd(0), Type::i64());
            const uint32_t argc = inst_ast.param_count;
            std::vector<Value*> args;
            for (size_t a = 0; a < argc; ++a) args.push_back(get_opd(1 + a));
            Value* key_id = lowering ? lowering->get_key_id(b, inst_ast.index) : b.build_iconst_i32(inst_ast.index);
            if (Value* site = (lowering ? lowering->prop_lowering().ic_site(b, inst_ast.ic_index) : nullptr)) {
                Value* argv = lowering->stage_argv(b, args);
                Value* argc_val = b.build_iconst_i32(static_cast<int32_t>(argc));
                res_val = b.build_call("bronze_call_method", Type::i64(), {recv, key_id, argc_val, argv, site});
            } else {
                Value* method = b.build_call("bronze_prop_get", Type::i64(), {recv, key_id, b.build_iconst_i64(0)});
                recv = ensure_type(get_opd(0), Type::i64());
                std::vector<Value*> dyn_args = {method, recv};
                for (Value* arg : args) dyn_args.push_back(ensure_type(arg, Type::i64()));
                res_val = build_call_dynamic(b, dyn_args, lowering);
            }
            break;
        }

        case BronzeOp::MethodCallSpread: {
            Value* this_val = ensure_type(get_opd(0), Type::i64());
            Value* args = ensure_type(get_opd(1), Type::i64());
            Value* key_id = lowering ? lowering->get_key_id(b, inst_ast.index) : b.build_iconst_i32(inst_ast.index);
            Value* site = lowering ? lowering->prop_lowering().ic_site(b, inst_ast.ic_index) : nullptr;
            Value* entry = site ? site : b.build_iconst_i64(0);
            res_val = b.build_call("bronze_call_method_spread", Type::i64(), {this_val, key_id, args, entry});
            break;
        }

        case BronzeOp::SuperCall: {
            Value* base_ctor = ensure_type(get_opd(0), Type::i64());
            Value* this_val = ensure_type(get_opd(1), Type::i64());
            uint32_t argc = inst_ast.param_count;
            if (argc <= 16) {
                std::vector<Value*> super_args = {base_ctor, this_val};
                for (size_t a = 0; a < argc; ++a) {
                    super_args.push_back(ensure_type(get_opd(2 + a), Type::i64()));
                }
                std::string helper = "bronze_super_call_" + std::to_string(argc);
                res_val = b.build_call(helper, Type::i64(), Span<Value* const>(super_args.data(), super_args.size()));
            } else {
                std::vector<Value*> args;
                for (size_t a = 0; a < argc; ++a) args.push_back(get_opd(2 + a));
                Value* argv = lowering->stage_argv(b, args);
                Value* argc_val = b.build_iconst_i32(static_cast<int32_t>(argc));
                res_val = b.build_call("bronze_super_call_n", Type::i64(), {base_ctor, this_val, argc_val, argv});
            }
            break;
        }

        case BronzeOp::SuperCallSpread: {
            Value* base_ctor = ensure_type(get_opd(0), Type::i64());
            Value* this_val = ensure_type(get_opd(1), Type::i64());
            Value* args = ensure_type(get_opd(2), Type::i64());
            res_val = b.build_call("bronze_super_call_spread", Type::i64(), {base_ctor, this_val, args});
            break;
        }

        case BronzeOp::SuperGet: {
            Value* proto = ensure_type(get_opd(0), Type::i64());
            Value* this_val = ensure_type(get_opd(1), Type::i64());
            Value* kidx = lowering ? lowering->get_key_id(b, inst_ast.index) : b.build_iconst_i32(inst_ast.index);
            res_val = b.build_call("bronze_super_get", Type::i64(), {proto, kidx, this_val});
            break;
        }

        case BronzeOp::SuperSet: {
            Value* proto = ensure_type(get_opd(0), Type::i64());
            Value* this_val = ensure_type(get_opd(1), Type::i64());
            Value* val = ensure_type(get_opd(2), Type::i64());
            Value* kidx = lowering ? lowering->get_key_id(b, inst_ast.index) : b.build_iconst_i32(inst_ast.index);
            Value* strict = b.build_iconst_i32(inst_ast.imm_i64 != 0 ? 1 : 0);
            b.build_call("bronze_super_set", Type::void_type(), {proto, kidx, this_val, val, strict});
            break;
        }

        case BronzeOp::CallDynamic: {
            Value* callee_val = ensure_type(get_opd(0), Type::i64());
            Value* this_val = ensure_type(get_opd(1), Type::i64());
            uint32_t argc = inst_ast.param_count;
            std::vector<Value*> dyn_args = {callee_val, this_val};
            for (size_t a = 0; a < argc; ++a) {
                dyn_args.push_back(ensure_type(get_opd(2 + a), Type::i64()));
            }
            res_val = build_call_dynamic(b, dyn_args, lowering);
            break;
        }

        case BronzeOp::DynamicCallSpread: {
            Value* callee = ensure_type(get_opd(0), Type::i64());
            Value* this_val = ensure_type(get_opd(1), Type::i64());
            Value* args = ensure_type(get_opd(2), Type::i64());
            res_val = b.build_call("bronze_dynamic_call_spread", Type::i64(), {callee, this_val, args});
            break;
        }

        case BronzeOp::FuncRef: {
            std::string callee = inst_ast.callee_name;
            if (!callee.empty() && std::isdigit(static_cast<unsigned char>(callee[0]))) {
                uint32_t f_idx = static_cast<uint32_t>(std::stoul(callee));
                if (lowering && lowering->current_ast() && f_idx < lowering->current_ast()->functions.size()) {
                    callee = lowering->current_ast()->functions[f_idx].name;
                }
            }
            callee = lowering ? lowering->resolve_callee(callee) : callee;
            Value* code_addr = b.build_func_addr("__wrapper_" + callee);
            uint32_t arity = 0;
            uint32_t length = 0;
            uint32_t name_key_val = 0xFFFFFFFFu;
            uint32_t fn_flags_val = 0x03;
            if (lowering) {
                auto it_meta = lowering->options().function_meta.find(callee);
                if (it_meta != lowering->options().function_meta.end()) {
                    fn_flags_val = it_meta->second.fn_flags;
                    name_key_val = it_meta->second.name_key;
                    length = it_meta->second.required_args;
                    arity = it_meta->second.adapt_arity;
                } else if (lowering->current_ast()) {
                    for (const auto& f : lowering->current_ast()->functions) {
                        if (f.name == callee) {
                            length = static_cast<uint32_t>(f.params.size());
                            arity = length;
                            break;
                        }
                    }
                }
            }
            Value* arity_val = b.build_iconst_i32(static_cast<int32_t>(arity));
            Value* length_val = b.build_iconst_i32(static_cast<int32_t>(length));
            Value* name_key = (name_key_val != 0xFFFFFFFFu && lowering)
                ? lowering->get_key_id(b, name_key_val)
                : b.build_iconst_i32(static_cast<int32_t>(0xFFFFFFFFu));
            Value* fn_flags = b.build_iconst_i32(static_cast<int32_t>(fn_flags_val));
            Value* slot_cell = b.build_iconst_i64(0);
            res_val = b.build_call("bronze_function_singleton", Type::i64(), {
                code_addr,
                arity_val,
                length_val,
                name_key,
                fn_flags,
                slot_cell
            });
            break;
        }

        case BronzeOp::CreateFunc: {
            std::string callee = lowering ? lowering->resolve_create_func_callee(inst_ast.callee_name) : inst_ast.callee_name;
            Value* code_addr = b.build_func_addr("__wrapper_" + callee);
            Value* arity_val = b.build_iconst_i32(static_cast<int32_t>(inst_ast.param_count));
            Value* env_val = ensure_type(get_opd(0), Type::i64());
            uint32_t length = 0;
            uint32_t name_key_val = 0xFFFFFFFFu;
            uint32_t fn_flags_val = 0x03;
            if (lowering) {
                auto it_meta = lowering->options().function_meta.find(callee);
                if (it_meta != lowering->options().function_meta.end()) {
                    fn_flags_val = it_meta->second.fn_flags;
                    name_key_val = it_meta->second.name_key;
                    length = it_meta->second.required_args;
                }
            }
            Value* length_val = b.build_iconst_i32(static_cast<int32_t>(length));
            Value* name_key = (name_key_val != 0xFFFFFFFFu && lowering)
                ? lowering->get_key_id(b, name_key_val)
                : b.build_iconst_i32(static_cast<int32_t>(0xFFFFFFFFu));
            Value* fn_flags = b.build_iconst_i32(static_cast<int32_t>(fn_flags_val));
            res_val = b.build_call("bronze_create_function", Type::i64(), {
                code_addr,
                arity_val,
                length_val,
                name_key,
                fn_flags,
                env_val
            });
            break;
        }

        default:
            return false;
    }

    return true;
}

} // namespace il2mir
