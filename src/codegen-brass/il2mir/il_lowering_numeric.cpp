// The dynamic (NaN-boxed) forms of the numeric ops: `+ - * / %`, unary `-`,
// `~`, `& | ^ << >> >>>`, `< <= > >=`, `==`, `++`/`--` (numeric.step) and
// ToNumeric. Each computes Numbers inline and hands everything else (a
// String, a BigInt, an object with valueOf, a non-finite operand of a bitwise
// op) to its runtime helper.
//
// EVIDENCE (il_abi.h kArithArm*, opt/speculate/evidence.cpp planArith): the
// helper's block is the instruction's slow arm, counted; in a build made to
// be harvested the inline arm marks kArithArmNumbers, kArithArmDouble0 /
// kArithArmDouble1 for an operand that was a Number but no int32 (a
// fraction, -0, NaN, an infinity or out of range), and, for the ops whose
// result can leave the int32 range, kArithArmResultDouble for a result that
// was no int32. A plain build marks none of them.

#include "il_lowering_dynamic.h"
#include "il_lowering.h"
#include "il_lowering_ops.h"
#include "il_abi.h"
#include <initializer_list>
#include <string>

namespace il2mir {

namespace {

Value* iconst64(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

// Whether the Number bits `bits` (an i64) are an int32's: in range, integral
// and not -0. An i32 0/1.
Value* is_int32_number(Builder& b, Value* bits) {
    Value* f = b.build_bitcast_f64_i64(bits);
    Value* exact = b.build_eq(b.build_sitofp_f64_i64(b.build_sext_i64(inline_to_int32(b, f))), f);
    Value* neg_zero = b.build_eq(bits, b.build_iconst_i64(INT64_MIN));
    return b.build_and(b.build_and(int32_convertible(b, bits), exact), b.build_xor(neg_zero, b.build_iconst_i32(1)));
}

// Marks `arm` on the way through when `is_int` (an i32) is false.
void mark_unless(PropertyLoweringHelper& prop, Builder& b, Value* is_int, uint32_t arm, const std::string& name) {
    BasicBlock* at = b.current_block();
    BasicBlock* mark = b.append_block(name + "_mark");
    BasicBlock* next = b.append_block(name + "_next");
    b.position_at_end(at);
    b.build_br_if(is_int, next, mark);
    b.position_at_end(mark);
    prop.arm_begin(b, arm);
    b.build_br(next);
    b.position_at_end(next);
}

// On the inline arm: kArithArmNumbers, and per operand (Number bits, in
// operand order) its double arm when it was no int32.
void mark_operand_kinds(PropertyLoweringHelper& prop, Builder& b, const std::string& prefix,
                        std::initializer_list<Value*> ops) {
    if (!prop.evidence_marked()) return;
    prop.arm_begin(b, kArithArmNumbers);
    uint32_t arm = kArithArmDouble0;
    for (Value* v : ops) {
        mark_unless(prop, b, is_int32_number(b, v), arm, prefix + "_k" + std::to_string(arm));
        ++arm;
    }
}

// kArithArmResultDouble when the f64 result `f` is no int32.
void mark_result_kind(PropertyLoweringHelper& prop, Builder& b, const std::string& prefix, Value* f) {
    if (!prop.evidence_marked()) return;
    mark_unless(prop, b, is_int32_number(b, box_f64_bits(b, f)), kArithArmResultDouble, prefix + "_r");
}

Value* both_numbers(Builder& b, Value* op0, Value* op1) {
    Value* max = iconst64(b, kBronzeNumberMaxBits);
    return b.build_and(b.build_ule(op0, max), b.build_ule(op1, max));
}

// The shape every form below shares: `fast` when `test`, else the helper's
// call as the slow arm; both join with an i64 (or `result_type`) result.
struct Arms {
    BasicBlock* fast;
    BasicBlock* merge;
    Value* result;
    std::string prefix;
};
Arms open_arms(PropertyLoweringHelper& prop, Builder& b, const char* tag, Value* test, const char* helper,
               std::initializer_list<Value*> args, Type result_type = Type::i64(), bool mask_bool = false) {
    BasicBlock* cur = b.current_block();
    Arms a;
    a.prefix = std::string(tag) + "_" + std::to_string(cur->parent()->next_block_id());
    a.fast = b.append_block(a.prefix + "_fast");
    BasicBlock* slow = b.append_block(a.prefix + "_slow");
    a.merge = b.append_block(a.prefix + "_merge");
    a.result = b.add_block_param(a.merge, result_type);
    b.position_at_end(cur);
    b.build_br_if(test, a.fast, slow);
    b.position_at_end(slow);
    prop.slow_arm(b);
    Value* called = b.build_call(helper, result_type, args);
    if (mask_bool) called = b.build_and(called, b.build_iconst_i32(1));
    b.build_br(a.merge, {called});
    b.position_at_end(a.fast);
    return a;
}

Value* close_arms(Builder& b, const Arms& a, Value* fast_result) {
    b.build_br(a.merge, {fast_result});
    b.position_at_end(a.merge);
    return a.result;
}

}  // namespace

// Whether the inline ToInt32 below applies to the Value (or f64) bits `bits`:
// a Number of magnitude below 2^63, whose truncation fptosi computes exactly
// and whose low 32 bits are then ToInt32. NaN, the infinities and every
// non-Number (a NaN-boxed tag, above 0x7FF0... once the sign is masked off)
// fail it.
Value* int32_convertible(Builder& b, Value* bits) {
    Value* abs_bits = b.build_and(bits, iconst64(b, 0x7FFFFFFFFFFFFFFFULL));
    return b.build_ult(abs_bits, iconst64(b, 0x43E0000000000000ULL));
}

// ToInt32 of the double `f`, where int32_convertible holds.
Value* inline_to_int32(Builder& b, Value* f) {
    return b.build_trunc_i32(b.build_fptosi_i64(f));
}

// `+ - * /` (`bronze_dynamic_*`): two Numbers compute as f64 and box the
// result, NaN canonicalized.
Value* lower_dynamic_arith(PropertyLoweringHelper& prop, Builder& b, BronzeOp op, Value* op0, Value* op1,
                           const char* helper) {
    const Arms a = open_arms(prop, b, "num", both_numbers(b, op0, op1), helper, {op0, op1});
    mark_operand_kinds(prop, b, a.prefix, {op0, op1});
    Value* f0 = b.build_bitcast_f64_i64(op0);
    Value* f1 = b.build_bitcast_f64_i64(op1);
    Value* f = op == BronzeOp::Add ? b.build_add(f0, f1)
             : op == BronzeOp::Sub ? b.build_sub(f0, f1)
             : op == BronzeOp::Mul ? b.build_mul(f0, f1)
                                   : b.build_sdiv(f0, f1);
    mark_result_kind(prop, b, a.prefix, f);
    return close_arms(b, a, box_f64_bits(b, f));
}

// `%`: a non-negative int32-valued Number by a positive one is an integer
// remainder (no -0, no NaN to make); anything else is bronze_dynamic_mod.
// The inline arm is kArithArmNumbers: the evidence that names the int form.
Value* lower_dynamic_mod(PropertyLoweringHelper& prop, Builder& b, Value* op0, Value* op1) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "mod_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* ints = b.append_block(prefix + "_ints");
    BasicBlock* fast = b.append_block(prefix + "_fast");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());
    slow->set_cold();

    b.position_at_end(cur);
    b.build_br_if(b.build_and(int32_convertible(b, op0), int32_convertible(b, op1)), ints, slow);
    b.position_at_end(ints);
    Value* f0 = b.build_bitcast_f64_i64(op0);
    Value* f1 = b.build_bitcast_f64_i64(op1);
    Value* i0 = inline_to_int32(b, f0);
    Value* i1 = inline_to_int32(b, f1);
    // Exact int32 values (the round trip), the dividend >= 0 and the divisor
    // > 0: the sign bit of the bits rules out -0 as well as the negatives.
    Value* exact = b.build_and(b.build_eq(b.build_sitofp_f64_i64(b.build_sext_i64(i0)), f0),
                               b.build_eq(b.build_sitofp_f64_i64(b.build_sext_i64(i1)), f1));
    Value* signs = b.build_and(b.build_sge(op0, b.build_iconst_i64(0)), b.build_sgt(i1, b.build_iconst_i32(0)));
    b.build_br_if(b.build_and(exact, signs), fast, slow);
    b.position_at_end(fast);
    if (prop.evidence_marked()) prop.arm_begin(b, kArithArmNumbers);
    Value* r = b.build_smod(i0, i1);
    b.build_br(merge, {b.build_bitcast_i64_f64(b.build_sitofp_f64_i64(b.build_sext_i64(r)))});

    b.position_at_end(slow);
    prop.slow_arm(b);
    b.build_br(merge, {b.build_call("bronze_dynamic_mod", Type::i64(), {op0, op1})});
    b.position_at_end(merge);
    return result;
}

// `& | ^ << >> >>>`: two Numbers whose ToInt32 is inline compute on i32 and
// box the Number result; anything else (a BigInt, a string, a huge or
// non-finite Number) takes the helper. `>>>`'s uint32 result is the one that
// can be no int32.
Value* lower_dynamic_bitwise(PropertyLoweringHelper& prop, Builder& b, BronzeOp op, Value* op0, Value* op1,
                             const char* helper) {
    const Arms a = open_arms(prop, b, "bit", b.build_and(int32_convertible(b, op0), int32_convertible(b, op1)),
                             helper, {op0, op1});
    mark_operand_kinds(prop, b, a.prefix, {op0, op1});
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
    Value* f = op == BronzeOp::UShr ? b.build_sitofp_f64_i64(b.build_zext_i64(v)) : b.build_sitofp_f64_i32(v);
    if (op == BronzeOp::UShr) mark_result_kind(prop, b, a.prefix, f);
    return close_arms(b, a, b.build_bitcast_i64_f64(f));
}

// `~x`: a Number whose ToInt32 is inline is `ToInt32(x) ^ -1`; a BigInt and
// everything else is bronze_dynamic_bitnot.
Value* lower_dynamic_bitnot(PropertyLoweringHelper& prop, Builder& b, Value* op0) {
    const Arms a = open_arms(prop, b, "bnot", int32_convertible(b, op0), "bronze_dynamic_bitnot", {op0});
    mark_operand_kinds(prop, b, a.prefix, {op0});
    Value* v = b.build_xor(inline_to_int32(b, b.build_bitcast_f64_i64(op0)), b.build_iconst_i32(-1));
    return close_arms(b, a, b.build_bitcast_i64_f64(b.build_sitofp_f64_i32(v)));
}

// Unary `-`: a Number's sign flipped (-0 for 0); a BigInt and everything else
// is bronze_dynamic_neg.
Value* lower_dynamic_neg(PropertyLoweringHelper& prop, Builder& b, Value* op0) {
    const Arms a = open_arms(prop, b, "neg", b.build_ule(op0, iconst64(b, kBronzeNumberMaxBits)),
                             "bronze_dynamic_neg", {op0});
    mark_operand_kinds(prop, b, a.prefix, {op0});
    Value* f = b.build_neg(b.build_bitcast_f64_i64(op0));
    mark_result_kind(prop, b, a.prefix, f);
    return close_arms(b, a, box_f64_bits(b, f));
}

// `< <= > >=`: two Numbers compare as doubles (the ordered compare, false at
// NaN, as IsLessThan answers); the helper owns ToPrimitive and the string and
// BigInt orders.
Value* lower_dynamic_relational(PropertyLoweringHelper& prop, Builder& b, BronzeOp op, Value* op0, Value* op1,
                                const char* helper) {
    const Arms a = open_arms(prop, b, "rel", both_numbers(b, op0, op1), helper, {op0, op1}, Type::i32(), true);
    mark_operand_kinds(prop, b, a.prefix, {op0, op1});
    Value* l = b.build_bitcast_f64_i64(op0);
    Value* r = b.build_bitcast_f64_i64(op1);
    Value* v = op == BronzeOp::RelLt ? b.build_slt(l, r)
             : op == BronzeOp::RelLe ? b.build_sle(l, r)
             : op == BronzeOp::RelGt ? b.build_sgt(l, r)
                                     : b.build_sge(l, r);
    return close_arms(b, a, v);
}

// `==`: two Numbers are equal as doubles (+0 == -0, NaN unequal); everything
// else is bronze_loose_eq.
Value* lower_dynamic_loose_eq(PropertyLoweringHelper& prop, Builder& b, Value* op0, Value* op1) {
    const Arms a = open_arms(prop, b, "leq", both_numbers(b, op0, op1), "bronze_loose_eq", {op0, op1}, Type::i32(),
                             true);
    mark_operand_kinds(prop, b, a.prefix, {op0, op1});
    return close_arms(b, a, b.build_eq(b.build_bitcast_f64_i64(op0), b.build_bitcast_f64_i64(op1)));
}

// `++` / `--` (numeric.step, `delta` +1 or -1): a Number plus the delta; a
// BigInt and anything else is bronze_numeric_step.
Value* lower_numeric_step(PropertyLoweringHelper& prop, Builder& b, Value* op0, int delta) {
    const Arms a = open_arms(prop, b, "numstep", b.build_ule(op0, iconst64(b, kBronzeNumberMaxBits)),
                             "bronze_numeric_step", {op0, b.build_iconst_i32(delta >= 0 ? 1 : 0)});
    mark_operand_kinds(prop, b, a.prefix, {op0});
    Value* sum = b.build_fadd(b.build_bitcast_f64_i64(op0), b.build_fconst_f64(delta >= 0 ? 1.0 : -1.0));
    mark_result_kind(prop, b, a.prefix, sum);
    return close_arms(b, a, box_f64_bits(b, sum));
}

// ToNumeric: a Number is itself; anything else is bronze_to_numeric.
Value* lower_dynamic_to_numeric(PropertyLoweringHelper& prop, Builder& b, Value* op0) {
    const Arms a = open_arms(prop, b, "tonum", b.build_ule(op0, iconst64(b, kBronzeNumberMaxBits)),
                             "bronze_to_numeric", {op0});
    mark_operand_kinds(prop, b, a.prefix, {op0});
    return close_arms(b, a, op0);
}

}  // namespace il2mir
