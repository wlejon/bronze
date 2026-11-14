#include "il_lowering_ops.h"
#include "il_lowering_dynamic.h"
#include "il_lowering.h"
#include "il_abi.h"
#include <cmath>
#include <cstdlib>

namespace il2mir {

namespace {

// Which operand of a strict.eq is known to be a Number (an f64, or a box of
// one), with the other a boxed Value or an f64: 0 or 1, else -1. An i32 is
// left out, since an IL bool is held in one too.
int strict_eq_number_side(IlLowering* lowering, const BronzeInstruction& inst, Value* op0, Value* op1) {
    if (!lowering || inst.operands.size() != 2) return -1;
    auto number = [&](int i, Value* v) { return v->type() == Type::f64() || lowering->is_number_value(inst.operands[i]); };
    auto value = [](Value* v) { return v->type() == Type::i64() || v->type() == Type::f64(); };
    if (!value(op0) || !value(op1)) return -1;
    if (number(1, op1)) return 1;
    if (number(0, op0)) return 0;
    return -1;
}

// ToInt32 of the Value or double `v` (`is_f64`): inline for a Number
// int32_convertible accepts, else inline bit-extraction for f64 (zero calls,
// so live XMM values in enclosing loops are never spilled across a call) or
// the helper for boxed values.
Value* lower_to_int32(Builder& b, Value* v, bool is_f64, bool raw_unbox = false, bool hot = false) {
    BasicBlock* cur = b.current_block();
    if (is_f64 && v && v->is_instruction()) {
        const brass::Instruction* def = v->defining_instruction();
        if (def) {
            if (def->opcode() == brass::Opcode::sitofp_f64_i32 && def->operand_count() == 1) {
                return def->operand(0);
            }
            if (def->opcode() == brass::Opcode::sitofp_f64_i64 && def->operand_count() == 1) {
                Value* z = def->operand(0);
                if (z && z->is_instruction()) {
                    const brass::Instruction* zdef = z->defining_instruction();
                    if (zdef && zdef->opcode() == brass::Opcode::zext_i64 &&
                        zdef->operand_count() == 1 && zdef->operand(0)->type() == Type::i32()) {
                        return zdef->operand(0);
                    }
                }
            }
            if (def->opcode() == brass::Opcode::fconst_f64 && std::isfinite(def->imm_f64()) &&
                std::fabs(def->imm_f64()) <= 2147483647.0) {
                return b.build_iconst_i32(
                    static_cast<int32_t>(static_cast<int64_t>(def->imm_f64())));
            }
        }
    }
    if (is_f64 && raw_unbox) {
        return inline_to_int32(b, v);
    }
    const std::string prefix = "toi32_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(prefix + "_fast");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i32());
    if (is_f64 || hot) slow->set_cold();

    b.position_at_end(cur);
    Value* bits = is_f64 ? b.build_bitcast_i64_f64(v) : v;
    b.build_br_if(int32_convertible(b, bits), fast, slow);

    b.position_at_end(slow);
    if (is_f64) {
        // ECMA-262 7.1.6 for |v| >= 2^63, NaN, or ±Inf:
        // For 2^63 <= |v| < 2^84 (0x43E0000000000000 <= abs_bits < 0x4530000000000000),
        // the low 32 bits of the integer magnitude are (mant << ((exp - 1075) & 63)).
        // For |v| >= 2^84, NaN, or ±Inf (abs_bits >= 0x4530000000000000), ToInt32 is 0.
        Value* abs_bits = b.build_and(bits, b.build_iconst_i64(0x7FFFFFFFFFFFFFFFULL));
        Value* in_range = b.build_ult(abs_bits, b.build_iconst_i64(0x4530000000000000ULL));
        Value* mant = b.build_or(b.build_and(abs_bits, b.build_iconst_i64(0x000FFFFFFFFFFFFFULL)),
                                 b.build_iconst_i64(0x0010000000000000ULL));
        Value* exp = b.build_lshr(abs_bits, b.build_iconst_i64(52));
        Value* shift = b.build_and(b.build_sub(exp, b.build_iconst_i64(1075)),
                                   b.build_iconst_i64(63));
        Value* mag = b.build_shl(mant, shift);
        Value* neg = b.build_sub(b.build_iconst_i64(0), mag);
        Value* is_neg = b.build_slt(v, b.build_fconst_f64(0.0));
        Value* signed_val = b.build_select(is_neg, neg, mag);
        Value* slow_i64 = b.build_select(in_range, signed_val, b.build_iconst_i64(0));
        b.build_br(merge, {b.build_trunc_i32(slow_i64)});
    } else {
        b.build_br(merge, {b.build_call("bronze_to_int32", Type::i32(), {v})});
    }

    b.position_at_end(fast);
    b.build_br(merge, {inline_to_int32(b, is_f64 ? v : b.build_bitcast_f64_i64(v))});

    b.position_at_end(merge);
    return result;
}

} // namespace

Value* box_f64_bits(Builder& b, Value* f) {
    Value* bits = b.build_bitcast_i64_f64(f);
    return b.build_select(b.build_ne(f, f), b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_CANONICAL_NAN_BITS)),
                          bits);
}

bool is_ops_il_op(BronzeOp op) {
    switch (op) {
        case BronzeOp::ConstF64:
        case BronzeOp::ConstI32:
        case BronzeOp::ConstBool:
        case BronzeOp::ConstUndefined:
        case BronzeOp::ConstNull:
        case BronzeOp::MathUnary:
        case BronzeOp::MathRandom:
        case BronzeOp::LayoutEpoch:
        case BronzeOp::GetNewTarget:
        case BronzeOp::Add:
        case BronzeOp::Sub:
        case BronzeOp::Mul:
        case BronzeOp::Div:
        case BronzeOp::Mod:
        case BronzeOp::Neg:
        case BronzeOp::BitAnd:
        case BronzeOp::BitOr:
        case BronzeOp::BitXor:
        case BronzeOp::Shl:
        case BronzeOp::Shr:
        case BronzeOp::UShr:
        case BronzeOp::BitNot:
        case BronzeOp::ToInt32:
        case BronzeOp::ToNumeric:
        case BronzeOp::NumericStep:
        case BronzeOp::RelLt:
        case BronzeOp::RelLe:
        case BronzeOp::RelGt:
        case BronzeOp::RelGe:
        case BronzeOp::StrictEq:
        case BronzeOp::LooseEq:
        case BronzeOp::CmpLt:
        case BronzeOp::CmpLe:
        case BronzeOp::CmpGt:
        case BronzeOp::CmpGe:
        case BronzeOp::CmpEq:
        case BronzeOp::CmpNe:
        case BronzeOp::NumTruthy:
        case BronzeOp::IsNumber:
        case BronzeOp::IsDenseArray:
        case BronzeOp::IsNumberArray:
        case BronzeOp::IsTypedArray:
        case BronzeOp::IsArray:
        case BronzeOp::IsPlainArray:
        case BronzeOp::IsNullish:
        case BronzeOp::TypeOf:
        case BronzeOp::InstanceOf:
        case BronzeOp::In:
        case BronzeOp::ClassExtend:
        case BronzeOp::PrivateNew:
        case BronzeOp::PrivateHas:
        case BronzeOp::PrivateGet:
        case BronzeOp::PrivateAdd:
        case BronzeOp::PrivateSet:
        case BronzeOp::PrivateMisuse:
            return true;
        default:
            return false;
    }
}

bool lower_ops_instruction(
    IlLowering* lowering,
    const BronzeInstruction& inst_ast,
    Builder& b,
    Function* /*fn*/,
    std::unordered_map<uint32_t, Value*>& val_map,
    Value*& res_val
) {
    Type res_type = lower_type(inst_ast.result_type);
    // A type-feedback hint (il.h `numberHint`): the Number arm falls through,
    // the generic path is cold. Not in a run-once block, whose sites are the
    // helper call alone.
    const bool hint = inst_ast.number_hint && lowering && !lowering->run_once();
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

    switch (inst_ast.op) {
        case BronzeOp::ConstF64:
            res_val = b.build_fconst_f64(inst_ast.imm_f64);
            return true;
        case BronzeOp::ConstI32:
            res_val = b.build_iconst_i32(static_cast<int32_t>(inst_ast.imm_i64));
            return true;
        case BronzeOp::ConstBool:
            res_val = b.build_iconst_i32(inst_ast.imm_bool ? 1 : 0);
            return true;
        case BronzeOp::ConstUndefined:
            res_val = b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag));
            return true;
        case BronzeOp::ConstNull:
            res_val = b.build_iconst_i64(static_cast<int64_t>(kNullTag));
            return true;
        case BronzeOp::MathUnary: {
            Value* op0 = get_opd(0);
            if (!op0) return false;
            op0 = lowering->ensure_type(op0, Type::f64(), b);
            // sqrt, abs, floor and ceil are single instructions with exactly
            // the IEEE results ECMA-262 asks for (-0, NaN and the infinities
            // included), so they need no libm call: in the N-body kernel the
            // call to glibc's sqrt was an eighth of the time.
            switch (inst_ast.imm_i64) {
                case 0: res_val = b.build_sqrt_f64(op0); return true;
                case 1: res_val = b.build_fabs_f64(op0); return true;
                case 2: res_val = b.build_floor_f64(op0); return true;
                case 3: res_val = b.build_ceil_f64(op0); return true;
                default: break;
            }
            // From sin on they are the runtime's kernels, the ones the
            // builtins answer with, so an inline call and the program's own
            // `Math.sin` give the same answer (il_op.h, MathUnaryFn).
            std::string_view fn = "trunc";
            switch (inst_ast.imm_i64) {
                case 4: fn = "trunc"; break;
                case 5: fn = "bronze_math_sin_f64"; break;
                case 6: fn = "bronze_math_cos_f64"; break;
                case 7: fn = "bronze_math_exp_f64"; break;
                case 8: fn = "bronze_math_log_f64"; break;
                case 9: fn = "bronze_math_tan_f64"; break;
                case 10: fn = "bronze_math_asin_f64"; break;
                case 11: fn = "bronze_math_acos_f64"; break;
                case 12: fn = "bronze_math_atan_f64"; break;
                case 16: fn = "bronze_math_round_f64"; break;
                case 13: {
                    Value* op1 = get_opd(1);
                    if (!op1) return false;
                    op1 = lowering->ensure_type(op1, Type::f64(), b);
                    res_val = b.build_call("bronze_math_atan2_f64", Type::f64(), {op0, op1});
                    return true;
                }
                case 14:
                case 15: {
                    // Max / Min: a fold over the operands, left to right.
                    res_val = op0;
                    for (size_t i = 1; i < inst_ast.operands.size(); ++i) {
                        Value* opi = get_opd(i);
                        if (!opi) return false;
                        res_val = build_min_max_f64(b, inst_ast.imm_i64 == 14, res_val,
                                                    lowering->ensure_type(opi, Type::f64(), b));
                    }
                    return true;
                }
                default: return false;
            }
            res_val = b.build_call(fn, Type::f64(), {op0});
            return true;
        }

        case BronzeOp::MathRandom: {
            // The native's own generator (builtin_math.cpp), so the sequence
            // is one whichever form each call site took.
            res_val = b.build_call("bronze_math_random_f64", Type::f64(), {});
            return true;
        }

        case BronzeOp::LayoutEpoch: {
            // The TLS word the proto-hit paths compare (il_ic_lowering.cpp),
            // converted exactly: it is even, positive and far below 2^53.
            Module* mod = b.current_block()->parent()->parent();
            Value* tls = (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                                             : b.build_call("bronze_tls_block_addr", Type::i64(), {});
            res_val = b.build_sitofp_f64_i64(b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF));
            return true;
        }

        case BronzeOp::GetNewTarget: {
            res_val = b.build_call("bronze_get_new_target", Type::i64(), {});
            return true;
        }

        case BronzeOp::Add: {
            Value* op0 = get_opd(0);
            Value* op1 = get_opd(1);
            if (!op0 || !op1) return false;
            if (res_type == Type::f64()) {
                op0 = lowering->ensure_type(op0, Type::f64(), b);
                op1 = lowering->ensure_type(op1, Type::f64(), b);
                res_val = b.build_add(op0, op1);
            } else if (res_type == Type::i64()) {
                op0 = lowering->ensure_type(op0, Type::i64(), b);
                op1 = lowering->ensure_type(op1, Type::i64(), b);
                res_val = lower_dynamic_arith(lowering, b, BronzeOp::Add, op0, op1, "bronze_dynamic_add",
                                              hint, val_map);
            } else {
                op0 = lowering->ensure_type(op0, Type::i32(), b);
                op1 = lowering->ensure_type(op1, Type::i32(), b);
                res_val = b.build_add(op0, op1);
            }
            return true;
        }

        case BronzeOp::Sub:
        case BronzeOp::Mul:
        case BronzeOp::Div: {
            Value* op0 = get_opd(0);
            Value* op1 = get_opd(1);
            if (!op0 || !op1) return false;
            if (res_type == Type::i64()) {
                op0 = lowering->ensure_type(op0, Type::i64(), b);
                op1 = lowering->ensure_type(op1, Type::i64(), b);
                const char* fn_name = (inst_ast.op == BronzeOp::Sub) ? "bronze_dynamic_sub" :
                                      (inst_ast.op == BronzeOp::Mul) ? "bronze_dynamic_mul" :
                                                                       "bronze_dynamic_div";
                res_val = lower_dynamic_arith(lowering, b, inst_ast.op, op0, op1, fn_name, hint, val_map);
            } else if (res_type == Type::f64()) {
                op0 = lowering->ensure_type(op0, Type::f64(), b);
                op1 = lowering->ensure_type(op1, Type::f64(), b);
                res_val = (inst_ast.op == BronzeOp::Sub) ? b.build_sub(op0, op1) :
                          (inst_ast.op == BronzeOp::Mul) ? b.build_mul(op0, op1) :
                                                           b.build_sdiv(op0, op1);
            } else {
                op0 = lowering->ensure_type(op0, Type::i32(), b);
                op1 = lowering->ensure_type(op1, Type::i32(), b);
                res_val = (inst_ast.op == BronzeOp::Sub) ? b.build_sub(op0, op1) :
                          (inst_ast.op == BronzeOp::Mul) ? b.build_mul(op0, op1) :
                                                           b.build_sdiv(op0, op1);
            }
            return true;
        }

        case BronzeOp::Mod: {
            Value* op0 = get_opd(0);
            Value* op1 = get_opd(1);
            if (!op0 || !op1) return false;
            if (res_type == Type::i64()) {
                op0 = lowering->ensure_type(op0, Type::i64(), b);
                op1 = lowering->ensure_type(op1, Type::i64(), b);
                res_val = b.build_call("bronze_dynamic_mod", Type::i64(), {op0, op1});
            } else if (res_type == Type::f64()) {
                op0 = lowering->ensure_type(op0, Type::f64(), b);
                op1 = lowering->ensure_type(op1, Type::f64(), b);
                res_val = b.build_call("bronze_f64_mod", Type::f64(), {op0, op1});
            } else {
                op0 = lowering->ensure_type(op0, Type::i32(), b);
                op1 = lowering->ensure_type(op1, Type::i32(), b);
                res_val = b.build_smod(op0, op1);
            }
            return true;
        }

        case BronzeOp::Neg: {
            Value* op0 = get_opd(0);
            if (!op0) return false;
            if (res_type == Type::i64()) {
                op0 = lowering->ensure_type(op0, Type::i64(), b);
                res_val = hint ? lower_hinted_neg(b, op0) : b.build_call("bronze_dynamic_neg", Type::i64(), {op0});
            } else if (res_type == Type::f64()) {
                op0 = lowering->ensure_type(op0, Type::f64(), b);
                res_val = b.build_neg(op0);
            } else {
                op0 = lowering->ensure_type(op0, Type::i32(), b);
                res_val = b.build_neg(op0);
            }
            return true;
        }

        case BronzeOp::BitAnd:
        case BronzeOp::BitOr:
        case BronzeOp::BitXor:
        case BronzeOp::Shl:
        case BronzeOp::Shr:
        case BronzeOp::UShr: {
            if (res_type == Type::i64()) {
                Value* op0 = lowering->ensure_type(get_opd(0), Type::i64(), b);
                Value* op1 = lowering->ensure_type(get_opd(1), Type::i64(), b);
                const char* helper =
                    inst_ast.op == BronzeOp::BitAnd ? "bronze_dynamic_bitand" :
                    inst_ast.op == BronzeOp::BitOr  ? "bronze_dynamic_bitor" :
                    inst_ast.op == BronzeOp::BitXor ? "bronze_dynamic_bitxor" :
                    inst_ast.op == BronzeOp::Shl    ? "bronze_dynamic_shl" :
                    inst_ast.op == BronzeOp::Shr    ? "bronze_dynamic_shr" :
                                                      "bronze_dynamic_ushr";
                res_val = lowering->run_once() ? b.build_call(helper, Type::i64(), {op0, op1})
                                               : lower_dynamic_bitwise(b, inst_ast.op, op0, op1, helper, hint);
                return true;
            }
            Value* op0 = lowering->ensure_type(get_opd(0), Type::i32(), b);
            Value* op1 = lowering->ensure_type(get_opd(1), Type::i32(), b);
            Value* r = nullptr;
            switch (inst_ast.op) {
                case BronzeOp::BitAnd: r = b.build_and(op0, op1); break;
                case BronzeOp::BitOr:  r = b.build_or(op0, op1); break;
                case BronzeOp::BitXor: r = b.build_xor(op0, op1); break;
                case BronzeOp::Shl: {
                    Value* count = b.build_and(op1, b.build_iconst_i32(31));
                    r = b.build_shl(op0, count);
                    break;
                }
                case BronzeOp::Shr: {
                    Value* count = b.build_and(op1, b.build_iconst_i32(31));
                    r = b.build_ashr(op0, count);
                    break;
                }
                default: { // UShr
                    Value* count = b.build_and(op1, b.build_iconst_i32(31));
                    r = b.build_lshr(op0, count);
                    break;
                }
            }
            if (res_type == Type::f64()) {
                if (inst_ast.op == BronzeOp::UShr) {
                    Value* zext = b.build_zext_i64(r);
                    res_val = b.build_sitofp_f64_i64(zext);
                } else {
                    res_val = b.build_sitofp_f64_i32(r);
                }
            } else {
                res_val = r;
            }
            return true;
        }
        case BronzeOp::BitNot: {
            if (res_type == Type::i64()) {
                Value* op0 = lowering->ensure_type(get_opd(0), Type::i64(), b);
                res_val = b.build_call("bronze_dynamic_bitnot", Type::i64(), {op0});
            } else {
                Value* op0 = lowering->ensure_type(get_opd(0), Type::i32(), b);
                Value* r = b.build_xor(op0, b.build_iconst_i32(-1));
                res_val = (res_type == Type::f64()) ? b.build_sitofp_f64_i32(r) : r;
            }
            return true;
        }

        case BronzeOp::ToInt32: {
            Value* op0 = get_opd(0);
            if (!op0) return false;
            if (op0->type() == Type::i32()) {
                res_val = op0;
            } else if (op0->type() == Type::f64()) {
                res_val = lower_to_int32(b, op0, /*is_f64=*/true, inst_ast.raw_unbox);
            } else {
                Value* d = lowering->ensure_type(op0, Type::i64(), b);
                res_val = lower_to_int32(b, d, /*is_f64=*/false, /*raw_unbox=*/false, hint);
            }
            return true;
        }
        case BronzeOp::ToNumeric: {
            Value* op0 = get_opd(0);
            if (!op0) return false;
            if (res_type == Type::f64()) {
                res_val = lowering->ensure_type(op0, Type::f64(), b);
            } else {
                Value* lhs = lowering->ensure_type(op0, Type::i64(), b);
                if (lowering->run_once()) {
                    res_val = b.build_call("bronze_to_numeric", Type::i64(), {lhs});
                } else {
                    BasicBlock* cur = b.current_block();
                    const std::string pfx = "tonum_" + std::to_string(cur->parent()->next_block_id());
                    BasicBlock* slow = b.append_block(pfx + "_slow");
                    BasicBlock* merge = b.append_block(pfx + "_merge");
                    Value* result = b.add_block_param(merge, Type::i64());
                    if (hint) slow->set_cold();
                    b.position_at_end(cur);
                    Value* is_num = b.build_ule(lhs, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
                    b.build_br_if(is_num, merge, {lhs}, slow, {});
                    b.position_at_end(slow);
                    b.build_br(merge, {b.build_call("bronze_to_numeric", Type::i64(), {lhs})});
                    b.position_at_end(merge);
                    res_val = result;
                }
            }
            return true;
        }
        case BronzeOp::NumericStep: {
            Value* op0 = get_opd(0);
            if (!op0) return false;
            Value* lhs = lowering->ensure_type(op0, Type::i64(), b);
            Value* is_inc = b.build_iconst_i32(inst_ast.imm_i64 >= 0 ? 1 : 0);
            if (lowering->run_once()) {
                res_val = b.build_call("bronze_numeric_step", Type::i64(), {lhs, is_inc});
            } else {
                BasicBlock* cur = b.current_block();
                const std::string pfx = "numstep_" + std::to_string(cur->parent()->next_block_id());
                BasicBlock* fast = b.append_block(pfx + "_fast");
                BasicBlock* slow = b.append_block(pfx + "_slow");
                BasicBlock* merge = b.append_block(pfx + "_merge");
                Value* result = b.add_block_param(merge, Type::i64());
                if (hint) slow->set_cold();
                b.position_at_end(cur);
                Value* is_num = b.build_ule(lhs, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
                b.build_br_if(is_num, fast, slow);
                b.position_at_end(slow);
                b.build_br(merge, {b.build_call("bronze_numeric_step", Type::i64(), {lhs, is_inc})});
                b.position_at_end(fast);
                Value* delta = b.build_fconst_f64(inst_ast.imm_i64 >= 0 ? 1.0 : -1.0);
                Value* sum = b.build_fadd(b.build_bitcast_f64_i64(lhs), delta);
                b.build_br(merge, {lowering->prop_lowering().box_number(b, sum)});
                b.position_at_end(merge);
                res_val = result;
            }
            return true;
        }

        case BronzeOp::RelLt:
        case BronzeOp::RelLe:
        case BronzeOp::RelGt:
        case BronzeOp::RelGe: {
            Value* op0 = lowering->ensure_type(get_opd(0), Type::i64(), b);
            Value* op1 = lowering->ensure_type(get_opd(1), Type::i64(), b);
            const char* helper = (inst_ast.op == BronzeOp::RelLt) ? "bronze_rel_lt" :
                                 (inst_ast.op == BronzeOp::RelLe) ? "bronze_rel_le" :
                                 (inst_ast.op == BronzeOp::RelGt) ? "bronze_rel_gt" : "bronze_rel_ge";
            res_val = lower_dynamic_relational(b, inst_ast.op, op0, op1, helper, lowering->run_once(), hint);
            lowering->note_boolean(res_val);
            return true;
        }

        case BronzeOp::StrictEq: {
            Value* op0 = get_opd(0);
            Value* op1 = get_opd(1);
            if (op0->type() == Type::f64() && op1->type() == Type::f64()) {
                res_val = b.build_eq(op0, op1);
            } else if (op0->type() == Type::i32() && op1->type() == Type::i32()) {
                res_val = b.build_eq(op0, op1);
            } else if (const int known = strict_eq_number_side(lowering, inst_ast, op0, op1); known >= 0) {
                // One side a Number: the other is equal exactly when it is a
                // Number of the same value. Every other value's bits are a
                // NaN as a double (tags sit above the Number range), so one
                // f64 compare answers both, NaN and -0 as IsStrictlyEqual has
                // them.
                Value* num = lowering->ensure_type(known == 0 ? op0 : op1, Type::f64(), b);
                Value* other = lowering->ensure_type(known == 0 ? op1 : op0, Type::i64(), b);
                res_val = b.build_eq(b.build_bitcast_f64_i64(other), num);
            } else {
                op0 = lowering->ensure_type(op0, Type::i64(), b);
                op1 = lowering->ensure_type(op1, Type::i64(), b);
                // il.h kStrictEqIdentityDispatch (1): either answer for two
                // operands with different bits is a correct path, so the bits
                // decide; one compare in place of the String arm's loads.
                constexpr int64_t kIdentityDispatch = 1;
                const uint32_t id0 = inst_ast.operands[0];
                const uint32_t id1 = inst_ast.operands[1];
                const bool lit0 = lowering->is_string_literal(id0);
                const bool lit1 = lowering->is_string_literal(id1);
                if (lowering->is_elided_typeof(id0) || lowering->is_elided_typeof(id1)) {
                    const bool elided0 = lowering->is_elided_typeof(id0);
                    Value* tag_val = elided0 ? op0 : op1;
                    std::string_view lit_str = lowering->string_literal_text(elided0 ? id1 : id0);
                    if (lit_str == "undefined") {
                        Value* masked = b.build_and(tag_val, b.build_iconst_i64(~1LL));
                        res_val = b.build_eq(masked, b.build_iconst_i64(BRONZE_ABI_TAG_UNDEFINED));
                    } else {
                        int64_t expected_tag =
                            (lit_str == "number")  ? BRONZE_ABI_TAG_INT32 :
                            (lit_str == "string")  ? BRONZE_ABI_TAG_STRING :
                            (lit_str == "boolean") ? BRONZE_ABI_TAG_BOOL :
                            (lit_str == "symbol")  ? BRONZE_ABI_TAG_SYMBOL :
                                                     BRONZE_ABI_TAG_BIGINT;
                        res_val = b.build_eq(tag_val, b.build_iconst_i64(expected_tag));
                    }
                } else if (inst_ast.imm_i64 == kIdentityDispatch ||
                           (lowering->is_typeof_value(id0) && lit1) ||
                           (lowering->is_typeof_value(id1) && lit0)) {
                    res_val = b.build_eq(op0, op1);
                } else if (!lowering->run_once() && (lit0 || lit1) &&
                           !is_bits_identity_constant(op0) && !is_bits_identity_constant(op1)) {
                    res_val = lit1 ? lower_strict_eq_string_literal(b, op0, op1)
                                   : lower_strict_eq_string_literal(b, op1, op0);
                } else {
                    res_val = lower_dynamic_strict_eq(b, op0, op1, lowering->run_once(), hint,
                                                      &lowering->prop_lowering());
                }
            }
            if (lowering) lowering->note_boolean(res_val);
            return true;
        }

        case BronzeOp::LooseEq: {
            Value* op0 = get_opd(0);
            Value* op1 = get_opd(1);
            if (op0->type() == Type::f64() && op1->type() == Type::f64()) {
                res_val = b.build_eq(op0, op1);
            } else if (op0->type() == Type::i32() && op1->type() == Type::i32()) {
                res_val = b.build_eq(op0, op1);
            } else {
                op0 = lowering->ensure_type(op0, Type::i64(), b);
                op1 = lowering->ensure_type(op1, Type::i64(), b);
                if (hint) {
                    res_val = lower_hinted_loose_eq(b, op0, op1);
                    if (lowering) lowering->note_boolean(res_val);
                } else {
                    res_val = b.build_call("bronze_loose_eq", Type::i32(), {op0, op1});
                    res_val = b.build_and(res_val, b.build_iconst_i32(1));
                }
            }
            return true;
        }

        case BronzeOp::CmpLt:
        case BronzeOp::CmpLe:
        case BronzeOp::CmpGt:
        case BronzeOp::CmpGe:
        case BronzeOp::CmpEq:
        case BronzeOp::CmpNe: {
            Value* op0 = get_opd(0);
            Value* op1 = get_opd(1);
            if (op0->type() == Type::f64() || op1->type() == Type::f64()) {
                op0 = lowering->ensure_type(op0, Type::f64(), b);
                op1 = lowering->ensure_type(op1, Type::f64(), b);
            } else if (op0->type() == Type::i64() || op1->type() == Type::i64()) {
                op0 = lowering->ensure_type(op0, Type::i64(), b);
                op1 = lowering->ensure_type(op1, Type::i64(), b);
            } else {
                op0 = lowering->ensure_type(op0, Type::i32(), b);
                op1 = lowering->ensure_type(op1, Type::i32(), b);
            }
            if (inst_ast.op == BronzeOp::CmpLt) res_val = b.build_slt(op0, op1);
            else if (inst_ast.op == BronzeOp::CmpLe) res_val = b.build_sle(op0, op1);
            else if (inst_ast.op == BronzeOp::CmpGt) res_val = b.build_sgt(op0, op1);
            else if (inst_ast.op == BronzeOp::CmpGe) res_val = b.build_sge(op0, op1);
            else if (inst_ast.op == BronzeOp::CmpEq) res_val = b.build_eq(op0, op1);
            else res_val = b.build_ne(op0, op1);
            return true;
        }

        case BronzeOp::NumTruthy: {
            // brass's f64 `ne` is the UNORDERED compare (it is CmpNe's, where
            // `NaN !== NaN` must be true), so on its own it calls NaN truthy.
            // ToBoolean wants the ORDERED "not 0": also require x == x, the
            // ordered equality that is false exactly at NaN.
            Value* op0 = lowering->ensure_type(get_opd(0), Type::f64(), b);
            Value* zero = b.build_fconst_f64(0.0);
            Value* nonZero = b.build_ne(op0, zero);
            Value* notNaN = b.build_eq(op0, op0);
            res_val = b.build_and(nonZero, notNaN);
            return true;
        }

        case BronzeOp::IsNumber: {
            // A double, or a box of one made here (an array's header length,
            // a typed element), is a Number without a test.
            Value* opd = get_opd(0);
            if (opd != nullptr && (opd->type() == Type::f64() || boxed_f64_source(opd) != nullptr)) {
                res_val = b.build_iconst_i32(1);
                return true;
            }
            // A read of a layout cell's double slot (il_property.h, NUMBER
            // FLAGS): a Number wherever its cell's guard held.
            if (Value* flag = lowering->prop_lowering().number_flag(opd)) {
                res_val = flag;
                return true;
            }
            Value* src = lowering->ensure_type(opd, Type::i64(), b);
            Value* limit = b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits));
            res_val = b.build_ule(src, limit);
            return true;
        }

        case BronzeOp::IsDenseArray: {
            if (!lowering->prop_lowering().enable_inlined_fastpaths() ||
                !lowering->prop_lowering().inline_sets()) {
                res_val = b.build_iconst_i32(0);
                return true;
            }
            Value* src = lowering->ensure_type(get_opd(0), Type::i64(), b);
            const auto max_index = static_cast<uint32_t>(inst_ast.imm_i64);
            res_val = lowering->prop_lowering().lower_is_dense_array(b, src, max_index);
            return true;
        }

        // Inline paths off, the claim answers no and the reads keep their
        // number tests.
        case BronzeOp::IsNumberArray: {
            if (!lowering->prop_lowering().enable_inlined_fastpaths()) {
                res_val = b.build_iconst_i32(0);
                return true;
            }
            Value* src = lowering->ensure_type(get_opd(0), Type::i64(), b);
            const auto max_index = static_cast<uint32_t>(inst_ast.imm_i64);
            // Behind an `is.dense_array` that covers `max_index` (a read run's
            // receiver test), the receiver is already an Array in bounds.
            const bool proven = lowering->is_proven_dense_array(inst_ast.operands[0], max_index);
            res_val = lowering->prop_lowering().lower_is_number_array(b, src, max_index, proven);
            lowering->note_number_array_claim(inst_ast.operands[0], max_index, res_val);
            return true;
        }

        // With the inline paths off, no versioned loop is entered: its typed
        // element ops are inline paths too.
        case BronzeOp::IsTypedArray: {
            if (!lowering->prop_lowering().enable_inlined_fastpaths()) {
                res_val = b.build_iconst_i32(0);
                return true;
            }
            Value* src = lowering->ensure_type(get_opd(0), Type::i64(), b);
            res_val = lowering->prop_lowering().lower_is_typed_array(b, src, inst_ast.imm_i64);
            return true;
        }

        // The elem ops it licenses are inline paths too.
        case BronzeOp::IsArray: {
            if (!lowering->prop_lowering().enable_inlined_fastpaths()) {
                res_val = b.build_iconst_i32(0);
                return true;
            }
            res_val = lowering->prop_lowering().lower_is_array(b, lowering->ensure_type(get_opd(0), Type::i64(), b));
            return true;
        }

        // Without the inline paths the guard always misses: its edge is then
        // the original call.
        case BronzeOp::IsPlainArray: {
            if (!lowering->prop_lowering().enable_inlined_fastpaths()) {
                res_val = b.build_iconst_i32(0);
                return true;
            }
            res_val = lowering->prop_lowering().lower_is_plain_array(
                b, lowering->ensure_type(get_opd(0), Type::i64(), b));
            return true;
        }

        case BronzeOp::IsNullish: {
            Value* op0 = lowering->ensure_type(get_opd(0), Type::i64(), b);
            Value* is_null = b.build_eq(op0, b.build_iconst_i64(static_cast<int64_t>(kNullTag)));
            Value* masked = b.build_and(op0, b.build_iconst_i64(~(1LL << BRONZE_ABI_VALUE_TAG_SHIFT)));
            Value* is_undef_or_hole = b.build_eq(masked, b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag)));
            res_val = b.build_or(is_null, is_undef_or_hole);
            if (lowering) lowering->note_boolean(res_val);
            return true;
        }

        case BronzeOp::TypeOf: {
            Value* op0 = get_opd(0);
            if (!op0) return false;
            Value* src = lowering->ensure_type(op0, Type::i64(), b);
            if (lowering && lowering->is_elided_typeof(inst_ast.result_id)) {
                Value* is_num = b.build_ule(src, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
                Value* raw_tag = b.build_lshr(src, b.build_iconst_i64(BRONZE_ABI_VALUE_TAG_SHIFT));
                res_val = b.build_select(is_num, b.build_iconst_i64(BRONZE_ABI_TAG_INT32), raw_tag);
                return true;
            }
            res_val = b.build_call("bronze_typeof", Type::i64(), {src});
            return true;
        }

        case BronzeOp::InstanceOf: {
            Value* op0 = lowering->ensure_type(get_opd(0), Type::i64(), b);
            Value* op1 = lowering->ensure_type(get_opd(1), Type::i64(), b);
            res_val = b.build_call("bronze_instanceof", Type::i32(), {op0, op1});
            res_val = b.build_and(res_val, b.build_iconst_i32(1));
            return true;
        }

        case BronzeOp::In: {
            Value* op1 = lowering->ensure_type(get_opd(1), Type::i64(), b);
            // The inline Array-element arm (il_elem_array.cpp); a key that is
            // not an index takes its helper.
            if (lowering->prop_lowering().enable_inlined_fastpaths()) {
                // A read of the same element on the true edge takes the slot
                // this loads (il_in_elem_fuse.cpp).
                if (lowering->in_loads_element(inst_ast.result_id)) {
                    Value* elem = nullptr;
                    res_val = lowering->prop_lowering().lower_has_index_array(
                        b, get_opd(0), op1, &elem, lowering->in_element_flags(inst_ast.result_id));
                    lowering->note_in_element(inst_ast.result_id, elem);
                    return true;
                }
                res_val = lowering->prop_lowering().lower_has_index_array(b, get_opd(0), op1);
                return true;
            }
            Value* op0 = lowering->ensure_type(get_opd(0), Type::i64(), b);
            res_val = b.build_call("bronze_has_property", Type::i32(), {op0, op1});
            res_val = b.build_and(res_val, b.build_iconst_i32(1));
            return true;
        }

        case BronzeOp::ClassExtend: {
            Value* sub = lowering->ensure_type(get_opd(0), Type::i64(), b);
            Value* sup = lowering->ensure_type(get_opd(1), Type::i64(), b);
            b.build_call("bronze_class_extends", Type::void_type(), {sub, sup});
            res_val = nullptr;
            return true;
        }

        case BronzeOp::PrivateNew: {
            res_val = b.build_call("bronze_private_new", Type::i64(), {});
            return true;
        }

        case BronzeOp::PrivateHas: {
            Value* table = lowering->ensure_type(get_opd(0), Type::i64(), b);
            Value* obj = lowering->ensure_type(get_opd(1), Type::i64(), b);
            uint32_t key_idx = inst_ast.string_literal.empty() ? inst_ast.index : lowering->find_key_constant(inst_ast.string_literal);
            Value* key_id = lowering->get_key_id(b, key_idx);
            res_val = b.build_call("bronze_private_has", Type::i32(), {table, obj, key_id});
            res_val = b.build_and(res_val, b.build_iconst_i32(1));
            return true;
        }

        case BronzeOp::PrivateGet: {
            Value* table = lowering->ensure_type(get_opd(0), Type::i64(), b);
            Value* obj = lowering->ensure_type(get_opd(1), Type::i64(), b);
            uint32_t key_idx = inst_ast.string_literal.empty() ? inst_ast.index : lowering->find_key_constant(inst_ast.string_literal);
            Value* key_id = lowering->get_key_id(b, key_idx);
            res_val = b.build_call("bronze_private_get", Type::i64(), {table, obj, key_id});
            return true;
        }

        case BronzeOp::PrivateAdd: {
            Value* table = lowering->ensure_type(get_opd(0), Type::i64(), b);
            Value* obj = lowering->ensure_type(get_opd(1), Type::i64(), b);
            Value* val = lowering->ensure_type(get_opd(2), Type::i64(), b);
            b.build_call("bronze_private_add", Type::void_type(), {table, obj, val});
            res_val = nullptr;
            return true;
        }

        case BronzeOp::PrivateSet: {
            Value* table = lowering->ensure_type(get_opd(0), Type::i64(), b);
            Value* obj = lowering->ensure_type(get_opd(1), Type::i64(), b);
            Value* val = lowering->ensure_type(get_opd(2), Type::i64(), b);
            uint32_t key_idx = inst_ast.string_literal.empty() ? inst_ast.index : lowering->find_key_constant(inst_ast.string_literal);
            Value* key_id = lowering->get_key_id(b, key_idx);
            b.build_call("bronze_private_set", Type::void_type(), {table, obj, val, key_id});
            res_val = nullptr;
            return true;
        }

        case BronzeOp::PrivateMisuse: {
            uint32_t key_idx = inst_ast.string_literal.empty() ? inst_ast.index : lowering->find_key_constant(inst_ast.string_literal);
            Value* key_id = lowering->get_key_id(b, key_idx);
            Value* code_val = b.build_iconst_i32(static_cast<int32_t>(inst_ast.imm_i64));
            res_val = b.build_call("bronze_private_misuse", Type::i64(), {key_id, code_val});
            return true;
        }

        default:
            return false;
    }
}

} // namespace il2mir
