#include "il_lowering_calls.h"
#include "il_lowering.h"
#include "il_abi.h"
#include <algorithm>
#include <cctype>
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

static Value* build_call_dynamic(Builder& b, const std::vector<Value*>& dyn_args, IlLowering* lowering) {
    size_t argc = dyn_args.size() > 2 ? dyn_args.size() - 2 : 0;
    if (argc <= 16) {
        std::string helper = "bronze_call_dynamic_" + std::to_string(argc);
        return b.build_call(helper, Type::i64(), Span<Value* const>(dyn_args.data(), dyn_args.size()));
    }
    Value* argv = lowering->stage_argv(b, std::vector<Value*>(dyn_args.begin() + 2, dyn_args.end()));
    Value* argc_val = b.build_iconst_i32(static_cast<int32_t>(argc));
    return b.build_call("bronze_call_dynamic_n", Type::i64(), {dyn_args[0], dyn_args[1], argc_val, argv});
}

namespace {

// The Math members a method call can speculate on (docs/il2mir.md §7): the
// function object's code pointer, which a moving GC never changes, and the
// machine op that computes the member exactly for a Number argument.
enum class BuiltinOp { None, Sqrt, Abs, Floor, Ceil };

struct BuiltinTarget {
    BuiltinOp op = BuiltinOp::None;
    bronze_fn_code code = nullptr;
};

BuiltinTarget builtin_target(const IlLowering* lowering, const BronzeInstruction& inst) {
    if (!lowering || inst.op != BronzeOp::MethodCall || inst.param_count != 1) return {};
    if (inst.ic_index == BronzeInstruction::kNoIcIndex) return {};
    const PropertyLoweringHelper& pl = const_cast<IlLowering*>(lowering)->prop_lowering();
    // The hit test compares against this process's address of the builtin:
    // only a module that runs here (not an AOT object) can hold it.
    if (!pl.feedback_driven() || !pl.in_process() || !pl.enable_inlined_fastpaths()) return {};
    const auto& keys = lowering->options().key_constants;
    if (inst.index >= keys.size()) return {};
    const std::string& key = keys[inst.index];
    if (key == "sqrt") return {BuiltinOp::Sqrt, &bronze_math_sqrt};
    if (key == "abs") return {BuiltinOp::Abs, &bronze_math_abs};
    if (key == "floor") return {BuiltinOp::Floor, &bronze_math_floor};
    if (key == "ceil") return {BuiltinOp::Ceil, &bronze_math_ceil};
    return {};
}

// `recv.key(arg)` where `key` names a speculated builtin: the method is
// read (the inline property path, its own site), then a CallTarget site
// tests that it is the builtin's function object and `arg` a Number. The
// fast path is the machine op, boxed with NaN canonicalized; the slow path
// calls the method already read, so the read happens once either way.
Value* lower_builtin_method_call(IlLowering* lowering, const BronzeInstruction& inst, Builder& b, Value* recv,
                                 Value* arg, Value* site) {
    const BuiltinTarget target = builtin_target(lowering, inst);
    if (target.op == BuiltinOp::None || !site) return nullptr;
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    Value* method = pl.lower_prop_get_mono(b, recv, inst.index, site);

    BasicBlock* cur = b.current_block();
    const std::string prefix = "builtin_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(prefix + "_fast");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    // A function object: the object tag, a header whose kind is Function,
    // and its code word the builtin's. A non-object reads a zeroed block.
    constexpr uint64_t kTagMask = 0xFFFF000000000000ULL;
    constexpr uint64_t kObjectTagBits = 0xFFF1000000000000ULL;
    constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
    constexpr int32_t kFunctionHeaderLow = (BRONZE_ABI_OBJ_FLAGS_FUNCTION << 16) | 0xFFF1;
    alignas(16) static const uint64_t kNotAnObject[4] = {};
    Value* is_obj = b.build_eq(b.build_and(method, b.build_iconst_i64(static_cast<int64_t>(kTagMask))),
                               b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    Value* ptr = b.build_and(method, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
    Value* base = b.build_select(
        is_obj, ptr, b.build_iconst_i64(static_cast<int64_t>(reinterpret_cast<uintptr_t>(&kNotAnObject[0]))));
    Value* is_fn = b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kFunctionHeaderLow));
    Value* code = b.build_load(Type::i64(), base, BRONZE_ABI_FN_CODE_OFFSET);
    Value* is_target =
        b.build_eq(code, b.build_iconst_i64(static_cast<int64_t>(reinterpret_cast<uintptr_t>(target.code))));
    Value* is_num = b.build_ule(arg, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
    Value* hit = b.build_and(b.build_and(is_obj, is_fn), b.build_and(is_target, is_num));
    lowering->spec().emit_branch(b, hit, fast, slow, SpecKind::CallTarget, pl.key_tag(inst.index));
    b.build_br(merge, {b.build_call("bronze_call_dynamic_1", Type::i64(), {method, recv, arg})});

    b.position_at_end(fast);
    Value* x = b.build_bitcast_f64_i64(arg);
    Value* r = target.op == BuiltinOp::Sqrt  ? b.build_sqrt_f64(x)
             : target.op == BuiltinOp::Abs   ? b.build_fabs(x)
             : target.op == BuiltinOp::Floor ? b.build_floor_f64(x)
                                             : b.build_ceil_f64(x);
    Value* bits = b.build_bitcast_i64_f64(r);
    Value* abs_bits = b.build_and(bits, b.build_iconst_i64(static_cast<int64_t>(0x7FFFFFFFFFFFFFFFULL)));
    Value* is_nan = b.build_ugt(abs_bits, b.build_iconst_i64(static_cast<int64_t>(0x7FF0000000000000ULL)));
    Value* boxed =
        b.build_select(is_nan, b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_CANONICAL_NAN_BITS)), bits);
    b.build_br(merge, {boxed});

    b.position_at_end(merge);
    return result;
}

// `recv.key(...)` whose callee lowering named (lower/direct_method_table.h).
// The name is the nearest declaration at or above the class inference gave
// the receiver, which is a GUESS: a subclass override, a monkey-patch or a
// receiver of another class entirely resolves `key` to another function. So
// the direct call runs only when the site's method IC answers the guessed
// function for this receiver — a plain object whose shape a DIRECT entry
// (way 0 or way 1) latched, with that entry's code pointer the callee's
// wrapper. Everything else, the first call that latches the entry included,
// takes the generic call, which dispatches on the receiver in hand.
Value* lower_direct_method_call(IlLowering* lowering, const BronzeInstruction& inst, Builder& b, Value* recv,
                                const std::vector<Value*>& args, Value* site, const std::string& callee,
                                Function* direct_fn) {
    constexpr int32_t kWord = 8;
    constexpr int64_t kDirectFormLimit = int64_t{1} << BRONZE_ABI_METHOD_IC_SLOT_SHIFT;
    PropertyLoweringHelper& pl = lowering->prop_lowering();

    BasicBlock* cur = b.current_block();
    const std::string prefix = "direct_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(prefix + "_fast");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    Value* plain = nullptr;
    Value* ptr = nullptr;
    Value* shape = nullptr;
    pl.receiver_shape(b, recv, plain, ptr, shape);
    Value* target = b.build_func_addr("__wrapper_" + callee);
    Value* way0 = b.build_and(
        b.build_and(b.build_eq(shape, b.build_load(Type::i64(), site, 0)),
                    b.build_ult(b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_ARITY_WORD * kWord),
                                b.build_iconst_i64(kDirectFormLimit))),
        b.build_eq(b.build_load(Type::ptr(), site, BRONZE_ABI_METHOD_IC_CODE_WORD * kWord), target));
    Value* way1 = b.build_and(
        b.build_eq(shape, b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_WAY1_SHAPE_WORD * kWord)),
        b.build_eq(b.build_load(Type::ptr(), site, BRONZE_ABI_METHOD_IC_WAY1_CODE_WORD * kWord), target));
    Value* hit = b.build_and(plain, b.build_or(way0, way1));
    lowering->spec().emit_branch(b, hit, fast, slow, SpecKind::Property, "()" + std::string(pl.key_tag(inst.index)));
    Value* argv = lowering->stage_argv(b, args);
    Value* argc_val = b.build_iconst_i32(static_cast<int32_t>(args.size()));
    Value* key_id = lowering->get_key_id(b, inst.index);
    b.build_br(merge, {b.build_call("bronze_call_method", Type::i64(), {recv, key_id, argc_val, argv, site})});

    b.position_at_end(fast);
    const auto& params = direct_fn->param_types();
    std::vector<Value*> call_args = {lowering->ensure_type(recv, params[0], b)};
    for (size_t p = 1; p < params.size(); ++p) {
        const Type pt = params[p];
        Value* a = p - 1 < args.size() ? args[p - 1] : nullptr;
        call_args.push_back(a ? lowering->ensure_type(a, pt, b)
                              : (pt == Type::f64() ? b.build_fconst_f64(0.0)
                                 : pt == Type::i32() ? b.build_iconst_i32(0)
                                 : lowering->ensure_type(b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag)), pt, b)));
    }
    Value* direct = b.build_call(callee, direct_fn->return_type(), Span<Value* const>(call_args.data(), call_args.size()));
    Value* boxed = direct_fn->return_type() == Type::void_type()
        ? b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag))
        : lowering->ensure_type(direct, Type::i64(), b);
    b.build_br(merge, {boxed});

    b.position_at_end(merge);
    return result;
}

} // namespace

bool method_call_is_speculated_builtin(const IlLowering* lowering, const BronzeInstruction& inst) {
    return builtin_target(lowering, inst).op != BuiltinOp::None;
}

bool lower_call_instruction(
    IlLowering* lowering,
    const BronzeInstruction& inst_ast,
    Builder& b,
    Function* fn,
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

        case BronzeOp::MethodCall: {
            Value* recv = ensure_type(get_opd(0), Type::i64());
            uint32_t argc = inst_ast.param_count;
            std::string callee = lowering ? lowering->resolve_callee(inst_ast.callee_name) : inst_ast.callee_name;
            if (!callee.empty() && std::isdigit(static_cast<unsigned char>(callee[0]))) {
                uint32_t f_idx = static_cast<uint32_t>(std::stoul(callee));
                if (lowering && lowering->current_ast() && f_idx < lowering->current_ast()->functions.size()) {
                    callee = lowering->resolve_callee(lowering->current_ast()->functions[f_idx].name);
                }
            }
            Function* direct_fn = (!callee.empty() && fn && fn->parent()) ? fn->parent()->get_function(callee) : nullptr;
            bool can_direct = false;
            if (direct_fn && lowering) {
                auto it_m = lowering->options().function_meta.find(callee);
                if (it_m != lowering->options().function_meta.end()) {
                    can_direct = it_m->second.needs_this && !it_m->second.needs_arguments &&
                                 !it_m->second.has_rest_param && !it_m->second.needs_env &&
                                 (direct_fn->param_types().size() == argc + 1);
                } else {
                    can_direct = (direct_fn->param_types().size() == argc + 1);
                }
            } else if (direct_fn) {
                can_direct = (direct_fn->param_types().size() == argc + 1);
            }

            if (!can_direct && method_call_is_speculated_builtin(lowering, inst_ast)) {
                if (Value* site = lowering->prop_lowering().ic_site(b, inst_ast.ic_index)) {
                    Value* arg = ensure_type(get_opd(1), Type::i64());
                    res_val = lower_builtin_method_call(lowering, inst_ast, b, recv, arg, site);
                    if (res_val) break;
                }
            }
            if (Value* site = (lowering ? lowering->prop_lowering().ic_site(b, inst_ast.ic_index) : nullptr)) {
                std::vector<Value*> args;
                for (size_t a = 0; a < argc; ++a) args.push_back(get_opd(1 + a));
                if (can_direct) {
                    res_val = lower_direct_method_call(lowering, inst_ast, b, recv, args, site, callee, direct_fn);
                    break;
                }
                Value* argv = lowering->stage_argv(b, args);
                Value* argc_val = b.build_iconst_i32(static_cast<int32_t>(argc));
                Value* key_id = lowering->get_key_id(b, inst_ast.index);
                res_val = b.build_call("bronze_call_method", Type::i64(),
                                       {recv, key_id, argc_val, argv, site});
            } else {
                Value* null_entry = b.build_iconst_i64(0);
                Value* key_id = lowering ? lowering->get_key_id(b, inst_ast.index) : b.build_iconst_i32(inst_ast.index);
                Value* method = b.build_call("bronze_prop_get", Type::i64(), {recv, key_id, null_entry});
                recv = ensure_type(get_opd(0), Type::i64());
                std::vector<Value*> dyn_args = {method, recv};
                for (size_t a = 0; a < argc; ++a) {
                    dyn_args.push_back(ensure_type(get_opd(1 + a), Type::i64()));
                }
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
