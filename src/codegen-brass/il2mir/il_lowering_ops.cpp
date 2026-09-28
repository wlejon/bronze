#include "il_lowering_ops.h"
#include "il_lowering.h"
#include "il_abi.h"

namespace il2mir {

namespace {

// A dynamic `+ - * /` (`bronze_dynamic_*`): two Numbers compute inline as f64
// and box the result, NaN canonicalized; anything else takes the helper. The
// test is a speculation site (il_speculation.h), which tier 2 turns into a
// guard where the helper never ran.
Value* lower_dynamic_arith(IlLowering* lowering, Builder& b, BronzeOp op, Value* op0, Value* op1,
                           const char* helper) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "num_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(prefix + "_fast");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    Value* max = b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits));
    Value* both = b.build_and(b.build_ule(op0, max), b.build_ule(op1, max));
    lowering->spec().emit_branch(b, both, fast, slow, SpecKind::Arith, helper);
    b.build_br(merge, {b.build_call(helper, Type::i64(), {op0, op1})});

    b.position_at_end(fast);
    Value* f0 = b.build_bitcast_f64_i64(op0);
    Value* f1 = b.build_bitcast_f64_i64(op1);
    Value* f = op == BronzeOp::Add ? b.build_add(f0, f1)
             : op == BronzeOp::Sub ? b.build_sub(f0, f1)
             : op == BronzeOp::Mul ? b.build_mul(f0, f1)
                                   : b.build_sdiv(f0, f1);
    Value* bits = b.build_bitcast_i64_f64(f);
    Value* abs_bits = b.build_and(bits, b.build_iconst_i64(static_cast<int64_t>(0x7FFFFFFFFFFFFFFFULL)));
    Value* is_nan = b.build_ugt(abs_bits, b.build_iconst_i64(static_cast<int64_t>(0x7FF0000000000000ULL)));
    Value* boxed = b.build_select(is_nan, b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_CANONICAL_NAN_BITS)), bits);
    b.build_br(merge, {boxed});

    b.position_at_end(merge);
    return result;
}

// Whether the inline ToInt32 below applies to the Value (or f64) bits `bits`:
// a Number of magnitude below 2^63, whose truncation fptosi computes exactly
// and whose low 32 bits are then ToInt32. NaN, the infinities and every
// non-Number (a NaN-boxed tag, above 0x7FF0... once the sign is masked off)
// fail it.
Value* int32_convertible(Builder& b, Value* bits) {
    Value* abs_bits = b.build_and(bits, b.build_iconst_i64(static_cast<int64_t>(0x7FFFFFFFFFFFFFFFULL)));
    return b.build_ult(abs_bits, b.build_iconst_i64(static_cast<int64_t>(0x43E0000000000000ULL)));
}

// ToInt32 of the double `f`, where int32_convertible holds.
Value* inline_to_int32(Builder& b, Value* f) {
    return b.build_trunc_i32(b.build_fptosi_i64(f));
}

// A dynamic `& | ^ << >> >>>`: two Numbers whose ToInt32 is inline compute on
// i32 and box the Number result; anything else (a BigInt, a string, a huge or
// non-finite Number) takes the helper.
Value* lower_dynamic_bitwise(Builder& b, BronzeOp op, Value* op0, Value* op1, const char* helper) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "bit_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(prefix + "_fast");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    b.build_br_if(b.build_and(int32_convertible(b, op0), int32_convertible(b, op1)), fast, slow);

    b.position_at_end(slow);
    b.build_br(merge, {b.build_call(helper, Type::i64(), {op0, op1})});

    b.position_at_end(fast);
    Value* l = inline_to_int32(b, b.build_bitcast_f64_i64(op0));
    Value* r = inline_to_int32(b, b.build_bitcast_f64_i64(op1));
    Value* v = nullptr;
    switch (op) {
        case BronzeOp::BitAnd: v = b.build_and(l, r); break;
        case BronzeOp::BitOr: v = b.build_or(l, r); break;
        case BronzeOp::BitXor: v = b.build_xor(l, r); break;
        case BronzeOp::Shl: v = b.build_shl(l, b.build_and(r, b.build_iconst_i32(31))); break;
        case BronzeOp::Shr: v = b.build_ashr(l, b.build_and(r, b.build_iconst_i32(31))); break;
        default: v = b.build_lshr(l, b.build_and(r, b.build_iconst_i32(31))); break;
    }
    // `>>>` is the one whose result is a uint32.
    Value* f = op == BronzeOp::UShr ? b.build_sitofp_f64_i64(b.build_zext_i64(v)) : b.build_sitofp_f64_i32(v);
    b.build_br(merge, {b.build_bitcast_i64_f64(f)});

    b.position_at_end(merge);
    return result;
}

// A dynamic `< <= > >=`: two Numbers compare inline as doubles (the ordered
// compare, false at NaN, as IsLessThan answers); anything else takes the
// helper, which owns ToPrimitive and the string and BigInt orders.
Value* lower_dynamic_relational(Builder& b, BronzeOp op, Value* op0, Value* op1, const char* helper) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "rel_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(prefix + "_fast");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i32());

    b.position_at_end(cur);
    Value* max = b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits));
    b.build_br_if(b.build_and(b.build_ule(op0, max), b.build_ule(op1, max)), fast, slow);

    b.position_at_end(slow);
    Value* called = b.build_and(b.build_call(helper, Type::i32(), {op0, op1}), b.build_iconst_i32(1));
    b.build_br(merge, {called});

    b.position_at_end(fast);
    Value* l = b.build_bitcast_f64_i64(op0);
    Value* r = b.build_bitcast_f64_i64(op1);
    Value* v = op == BronzeOp::RelLt ? b.build_slt(l, r)
             : op == BronzeOp::RelLe ? b.build_sle(l, r)
             : op == BronzeOp::RelGt ? b.build_sgt(l, r)
                                     : b.build_sge(l, r);
    b.build_br(merge, {v});

    b.position_at_end(merge);
    return result;
}

// ToInt32 of the Value or double `v` (`is_f64`): inline for a Number
// int32_convertible accepts, else the helper.
Value* lower_to_int32(Builder& b, Value* v, bool is_f64) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "toi32_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(prefix + "_fast");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i32());

    b.position_at_end(cur);
    Value* bits = is_f64 ? b.build_bitcast_i64_f64(v) : v;
    b.build_br_if(int32_convertible(b, bits), fast, slow);

    b.position_at_end(slow);
    b.build_br(merge, {b.build_call(is_f64 ? "bronze_to_int32_f64" : "bronze_to_int32", Type::i32(), {v})});

    b.position_at_end(fast);
    b.build_br(merge, {inline_to_int32(b, is_f64 ? v : b.build_bitcast_f64_i64(v))});

    b.position_at_end(merge);
    return result;
}

} // namespace

bool is_ops_il_op(BronzeOp op) {
    switch (op) {
        case BronzeOp::ConstF64:
        case BronzeOp::ConstI32:
        case BronzeOp::ConstBool:
        case BronzeOp::ConstUndefined:
        case BronzeOp::ConstNull:
        case BronzeOp::MathUnary:
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
            std::string_view fn = "sqrt";
            switch (inst_ast.imm_i64) {
                case 0: fn = "sqrt"; break;
                case 1: fn = "fabs"; break;
                case 2: fn = "floor"; break;
                case 3: fn = "ceil"; break;
                case 4: fn = "trunc"; break;
                case 5: fn = "sin"; break;
                case 6: fn = "cos"; break;
                default: break;
            }
            res_val = b.build_call(fn, Type::f64(), {op0});
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
                res_val = lower_dynamic_arith(lowering, b, BronzeOp::Add, op0, op1, "bronze_dynamic_add");
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
                res_val = lower_dynamic_arith(lowering, b, inst_ast.op, op0, op1, fn_name);
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
                res_val = b.build_call("bronze_dynamic_neg", Type::i64(), {op0});
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
                res_val = lower_dynamic_bitwise(b, inst_ast.op, op0, op1, helper);
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
                res_val = lower_to_int32(b, op0, /*is_f64=*/true);
            } else {
                Value* d = lowering->ensure_type(op0, Type::i64(), b);
                res_val = lower_to_int32(b, d, /*is_f64=*/false);
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
                res_val = b.build_call("bronze_to_numeric", Type::i64(), {lhs});
            }
            return true;
        }
        case BronzeOp::NumericStep: {
            Value* op0 = get_opd(0);
            if (!op0) return false;
            Value* lhs = lowering->ensure_type(op0, Type::i64(), b);
            Value* is_inc = b.build_iconst_i32(inst_ast.imm_i64 >= 0 ? 1 : 0);
            res_val = b.build_call("bronze_numeric_step", Type::i64(), {lhs, is_inc});
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
            res_val = lower_dynamic_relational(b, inst_ast.op, op0, op1, helper);
            return true;
        }

        case BronzeOp::StrictEq: {
            Value* op0 = get_opd(0);
            Value* op1 = get_opd(1);
            if (op0->type() == Type::f64() && op1->type() == Type::f64()) {
                res_val = b.build_eq(op0, op1);
            } else if (op0->type() == Type::i32() && op1->type() == Type::i32()) {
                res_val = b.build_eq(op0, op1);
            } else {
                op0 = lowering->ensure_type(op0, Type::i64(), b);
                op1 = lowering->ensure_type(op1, Type::i64(), b);
                res_val = b.build_call("bronze_strict_eq", Type::i32(), {op0, op1});
                res_val = b.build_and(res_val, b.build_iconst_i32(1));
            }
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
                res_val = b.build_call("bronze_loose_eq", Type::i32(), {op0, op1});
                res_val = b.build_and(res_val, b.build_iconst_i32(1));
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
            Value* src = lowering->ensure_type(get_opd(0), Type::i64(), b);
            Value* limit = b.build_iconst_i64(static_cast<int64_t>(0xFFF0000000000000ULL));
            res_val = b.build_ule(src, limit);
            return true;
        }

        case BronzeOp::IsDenseArray: {
            res_val = b.build_iconst_i32(0);
            return true;
        }

        case BronzeOp::IsNullish: {
            Value* op0 = lowering->ensure_type(get_opd(0), Type::i64(), b);
            res_val = b.build_call("bronze_is_nullish", Type::i32(), {op0});
            res_val = b.build_and(res_val, b.build_iconst_i32(1));
            return true;
        }

        case BronzeOp::TypeOf: {
            Value* op0 = get_opd(0);
            if (!op0) return false;
            Value* src = lowering->ensure_type(op0, Type::i64(), b);
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
            Value* op0 = lowering->ensure_type(get_opd(0), Type::i64(), b);
            Value* op1 = lowering->ensure_type(get_opd(1), Type::i64(), b);
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
