// The dynamic (NaN-boxed) forms of the arithmetic, bitwise, equality,
// relational and truthiness ops: an inline arm for the operands whose answer
// is in their bits, and the runtime helper for the rest.
//
// `hot` is a type-feedback hint (il.h `numberHint`): every operand the
// profiled run saw here was a Number. The Number arm is then the
// fall-through and every other arm, helper call included, is a cold block
// that brass's block layout sinks to the end of the function. A wrong hint
// moves blocks and changes no answer.

#include "il_lowering_dynamic.h"
#include "il_lowering.h"
#include "il_lowering_ops.h"
#include "il_abi.h"
#include <vector>

namespace il2mir {

// A dynamic `+ - * /` (`bronze_dynamic_*`): two Numbers compute inline as f64
// and box the result, NaN canonicalized; anything else takes the helper. The
// test is a speculation site (il_speculation.h), which tier 2 turns into a
// guard where the helper never ran.
Value* lower_dynamic_arith(IlLowering* lowering, Builder& b, BronzeOp op, Value* op0, Value* op1,
                           const char* helper, bool hot) {
    if (lowering->run_once()) return b.build_call(helper, Type::i64(), {op0, op1});
    BasicBlock* cur = b.current_block();
    const std::string prefix = "num_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(prefix + "_fast");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());
    if (hot) slow->set_cold();

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
    b.build_br(merge, {box_f64_bits(b, f)});

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
Value* lower_dynamic_bitwise(Builder& b, BronzeOp op, Value* op0, Value* op1, const char* helper, bool hot) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "bit_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(prefix + "_fast");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());
    if (hot) slow->set_cold();

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

bool is_bits_identity_constant(const Value* v) {
    if (!v || !v->is_instruction()) return false;
    const brass::Instruction* def = v->defining_instruction();
    if (!def) return false;
    if (def->opcode() == brass::Opcode::iconst_i64) {
        const uint64_t tag = static_cast<uint64_t>(def->imm_i64()) >> BRONZE_ABI_VALUE_TAG_SHIFT;
        return tag == BRONZE_ABI_TAG_UNDEFINED || tag == BRONZE_ABI_TAG_NULL || tag == BRONZE_ABI_TAG_BOOL;
    }
    // A boxed Boolean as il_lowering_inst.cpp's Box builds it, `zext(b & 1) |
    // kBoolTag`, constant or not (`update === true` against a bool parameter
    // boxed for its default): its bits are one of the two Boolean values, which
    // no Number, String or BigInt has, so `===` against it is the bits'
    // compare exactly as against a Boolean constant.
    if (def->opcode() != brass::Opcode::or_ || def->operand_count() != 2) return false;
    const Value* lhs = def->operand(0);
    const Value* rhs = def->operand(1);
    const brass::Instruction* zext = lhs->is_instruction() ? lhs->defining_instruction() : nullptr;
    const brass::Instruction* tag = rhs->is_instruction() ? rhs->defining_instruction() : nullptr;
    if (!zext || !tag || zext->opcode() != brass::Opcode::zext_i64 || tag->opcode() != brass::Opcode::iconst_i64 ||
        static_cast<uint64_t>(tag->imm_i64()) != kBoolTag) {
        return false;
    }
    // The zero-extended operand must be a 0/1: Box ANDs it with 1 first.
    const Value* low = zext->operand(0);
    const brass::Instruction* masked = low->is_instruction() ? low->defining_instruction() : nullptr;
    if (!masked || masked->opcode() != brass::Opcode::and_ || masked->operand_count() != 2) return false;
    const Value* one = masked->operand(1);
    const brass::Instruction* one_def = one->is_instruction() ? one->defining_instruction() : nullptr;
    return one_def && one_def->opcode() == brass::Opcode::iconst_i32 && one_def->imm_i64() == 1;
}

// Whether two String headers with different bits are proven to hold different
// text (lengths, or both memoized hashes and neither a builder), as an i32; 0
// means the contents compare must decide. See the strings arm below.
Value* build_strings_differ(Builder& b, Value* s0, Value* s1) {
    Value* len_differ = b.build_ne(b.build_load(Type::i32(), s0, BRONZE_ABI_STRING_LENGTH_OFFSET),
                                   b.build_load(Type::i32(), s1, BRONZE_ABI_STRING_LENGTH_OFFSET));
    Value* f0 = b.build_load(Type::i32(), s0, BRONZE_ABI_STRING_FLAGS_OFFSET);
    Value* f1 = b.build_load(Type::i32(), s1, BRONZE_ABI_STRING_FLAGS_OFFSET);
    Value* hashed_bit = b.build_iconst_i32(BRONZE_ABI_STRING_HASHED_BIT);
    Value* both_hashed = b.build_ne(b.build_and(b.build_and(f0, f1), hashed_bit), b.build_iconst_i32(0));
    // The heap header's low word is (flags << 16) | tag.
    static_assert(BRONZE_ABI_OBJ_FLAGS_OFFSET == 2, "the kind flags are the header word's high half");
    Value* h0 = b.build_load(Type::i32(), s0, 0);
    Value* h1 = b.build_load(Type::i32(), s1, 0);
    Value* builder = b.build_iconst_i32(static_cast<int32_t>(BRONZE_ABI_STRING_BUILDER_BIT << 16));
    Value* no_builder = b.build_eq(b.build_and(b.build_or(h0, h1), builder), b.build_iconst_i32(0));
    Value* hash_mask = b.build_iconst_i32(static_cast<int32_t>(BRONZE_ABI_STRING_HASH_MASK));
    Value* hash_differ = b.build_ne(b.build_and(f0, hash_mask), b.build_and(f1, hash_mask));
    return b.build_or(len_differ, b.build_and(b.build_and(both_hashed, no_builder), hash_differ));
}

// A dynamic `===`, bronze_strict_eq without the call wherever its answer is
// the bits'. Against an undefined, null or boolean constant it is a plain
// compare: no Number, String or BigInt has those bits. Otherwise two Numbers
// compare as doubles (NaN !== NaN, +0 === -0), equal bits are equal, and only
// two Strings or two BigInts with different bits need the helper's contents
// compare; any other pair with different bits is false.
Value* lower_dynamic_strict_eq(Builder& b, Value* op0, Value* op1, bool helper_only, bool hot) {
    if (is_bits_identity_constant(op0) || is_bits_identity_constant(op1)) return b.build_eq(op0, op1);
    if (helper_only) {
        return b.build_and(b.build_call("bronze_strict_eq", Type::i32(), {op0, op1}), b.build_iconst_i32(1));
    }
    BasicBlock* cur = b.current_block();
    const std::string prefix = "seq_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* num = b.append_block(prefix + "_num");
    BasicBlock* other = b.append_block(prefix + "_other");
    BasicBlock* strings = b.append_block(prefix + "_strings");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i32());
    if (hot) {
        other->set_cold();
        strings->set_cold();
        slow->set_cold();
    }

    b.position_at_end(cur);
    Value* max = b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits));
    b.build_br_if(b.build_and(b.build_ule(op0, max), b.build_ule(op1, max)), num, other);

    b.position_at_end(num);
    b.build_br(merge, {b.build_eq(b.build_bitcast_f64_i64(op0), b.build_bitcast_f64_i64(op1))});

    b.position_at_end(other);
    Value* same_bits = b.build_eq(op0, op1);
    Value* shift = b.build_iconst_i64(BRONZE_ABI_VALUE_TAG_SHIFT);
    Value* tag0 = b.build_lshr(op0, shift);
    Value* tag1 = b.build_lshr(op1, shift);
    Value* by_contents = b.build_or(b.build_eq(tag0, b.build_iconst_i64(BRONZE_ABI_TAG_STRING)),
                                    b.build_eq(tag0, b.build_iconst_i64(BRONZE_ABI_TAG_BIGINT)));
    Value* needs_helper = b.build_and(b.build_and(by_contents, b.build_eq(tag0, tag1)),
                                      b.build_eq(same_bits, b.build_iconst_i32(0)));
    b.build_br_if(needs_helper, strings, {}, merge, {same_bits});

    // Two Strings with different bits differ when their lengths do, or when
    // both carry a memoized hash and the hashes do (StringHeader::hash is of
    // the code units, so it is the same for either representation of the same
    // text). A builder's memo is not trusted: it is the one string whose text
    // can still grow. Anything else is the helper's contents compare.
    b.position_at_end(strings);
    {
        Value* is_string = b.build_eq(tag0, b.build_iconst_i64(BRONZE_ABI_TAG_STRING));
        Value* mask = b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_VALUE_PAYLOAD_MASK));
        // A BigInt pair reads nothing: both bases are then the first's own
        // payload, whose words are never compared for an answer.
        Value* s0 = b.build_and(op0, mask);
        Value* s1 = b.build_select(is_string, b.build_and(op1, mask), s0);
        Value* differ = build_strings_differ(b, s0, s1);
        b.build_br_if(b.build_and(is_string, differ), merge, {b.build_iconst_i32(0)}, slow, {});
    }

    b.position_at_end(slow);
    b.build_br(merge, {b.build_and(b.build_call("bronze_strict_eq", Type::i32(), {op0, op1}),
                                   b.build_iconst_i32(1))});

    b.position_at_end(merge);
    return result;
}

// A dynamic `< <= > >=`: two Numbers compare inline as doubles (the ordered
// compare, false at NaN, as IsLessThan answers); anything else takes the
// helper, which owns ToPrimitive and the string and BigInt orders.
Value* lower_dynamic_relational(Builder& b, BronzeOp op, Value* op0, Value* op1, const char* helper,
                                bool helper_only, bool hot) {
    if (helper_only) return b.build_and(b.build_call(helper, Type::i32(), {op0, op1}), b.build_iconst_i32(1));
    BasicBlock* cur = b.current_block();
    const std::string prefix = "rel_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(prefix + "_fast");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i32());
    if (hot) slow->set_cold();

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

// ToBoolean of a dynamic value, bronze_truthy without the call for every
// operand whose answer is in its bits: a Number is true unless it is +-0 or
// NaN, an Int32 or Boolean unless its payload is 0, undefined and null are
// false and an Object is true (bronze has no [[IsHTMLDDA]] object). Strings,
// BigInts and the hole read memory or are the helper's.
//
// Without a hint the tests are a ladder, each its own branch: a Boolean,
// then an Object, then undefined and null, then the Numbers and Int32s, then
// the helper. A hint from a type-feedback profile (`number_hint`, or the
// kTruth* `kinds` it saw) puts tests for those values first, on the
// fall-through, and the whole ladder after them in cold blocks; any value
// still gets its answer there.
Value* lower_dynamic_truthy(Builder& b, Value* v, bool helper_only, bool number_hint, uint8_t kinds) {
    if (helper_only) return b.build_and(b.build_call("bronze_unbox_bool", Type::i32(), {v}), b.build_iconst_i32(1));
    BasicBlock* cur = b.current_block();
    const std::string prefix = "truthy_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i32());
    BasicBlock* classify = b.append_block(prefix + "_classify");
    BasicBlock* not_obj = b.append_block(prefix + "_notobj");
    BasicBlock* not_undef = b.append_block(prefix + "_notundef");
    BasicBlock* not_nullish = b.append_block(prefix + "_notnullish");
    if (number_hint) kinds = 0;
    BasicBlock* ladder = number_hint || kinds != 0 ? b.append_block(prefix + "_ladder") : nullptr;
    // One block per hinted kind after the first, each the previous test's
    // miss edge (made here: appending a block moves the builder).
    std::vector<BasicBlock*> hinted;
    for (uint8_t k : {kTruthBool, kTruthObject, kTruthUndefined, kTruthNull}) {
        if ((kinds & k) != 0 && !hinted.empty()) hinted.push_back(b.append_block(prefix + "_hint"));
        if ((kinds & k) != 0 && hinted.empty()) hinted.push_back(cur);
    }
    b.position_at_end(cur);

    auto bits = [&](uint64_t x) { return b.build_iconst_i64(static_cast<int64_t>(x)); };
    auto is_object = [&] {
        return b.build_eq(b.build_lshr(v, bits(BRONZE_ABI_VALUE_TAG_SHIFT)), bits(BRONZE_ABI_TAG_OBJECT));
    };
    // The two Boolean values are exactly kBoolTag | 0 and kBoolTag | 1, so
    // `v - kBoolTag < 2` is "v is a Boolean" and its low bit is its answer.
    auto is_bool = [&] { return b.build_ult(b.build_sub(v, bits(kBoolTag)), bits(2)); };
    auto bool_answer = [&] { return b.build_and(b.build_trunc_i32(v), b.build_iconst_i32(1)); };
    if (number_hint) {
        // A profile saw only Numbers here (`if (count)`, `while (n)`).
        Value* abs = b.build_and(v, bits(0x7FFFFFFFFFFFFFFFULL));
        Value* num_true = b.build_ult(b.build_sub(abs, bits(1)), bits(0x7FF0000000000000ULL));
        b.build_br_if(b.build_ule(v, bits(kBronzeNumberMaxBits)), merge, {num_true}, ladder, {});
    } else if (kinds != 0) {
        // Only Booleans, objects and null / undefined (`if (this.visible)`,
        // `if (material.map)`, the join of `a && b`): each kind seen is one
        // test, in that order, and its answer.
        size_t i = 0;
        for (uint8_t k : {kTruthBool, kTruthObject, kTruthUndefined, kTruthNull}) {
            if ((kinds & k) == 0) continue;
            b.position_at_end(hinted[i]);
            BasicBlock* miss = i + 1 < hinted.size() ? hinted[i + 1] : ladder;
            ++i;
            if (k == kTruthBool) {
                b.build_br_if(is_bool(), merge, {bool_answer()}, miss, {});
            } else if (k == kTruthObject) {
                b.build_br_if(is_object(), merge, {b.build_iconst_i32(1)}, miss, {});
            } else {
                const uint64_t tag = k == kTruthUndefined ? kUndefinedTag : kNullTag;
                b.build_br_if(b.build_eq(v, bits(tag)), merge, {b.build_iconst_i32(0)}, miss, {});
            }
        }
    }
    if (ladder) {
        b.position_at_end(ladder);
        for (BasicBlock* bb : {ladder, slow, classify, not_obj, not_undef, not_nullish}) bb->set_cold();
    }
    // A Boolean first: most dynamic conditions are a flag field
    // (`if (this.matrixAutoUpdate)`) that only ever holds true or false.
    b.build_br_if(is_bool(), merge, {bool_answer()}, not_obj, {});
    // Then an Object and the two nullish values, each on a branch of its own:
    // the next most common conditions are presence tests (`if (material.map)`,
    // `object.parent !== null && ...` written as `if (object.parent)`).
    b.position_at_end(not_obj);
    b.build_br_if(is_object(), merge, {b.build_iconst_i32(1)}, not_undef, {});
    b.position_at_end(not_undef);
    b.build_br_if(b.build_eq(v, bits(kUndefinedTag)), merge, {b.build_iconst_i32(0)}, not_nullish, {});
    b.position_at_end(not_nullish);
    b.build_br_if(b.build_eq(v, bits(kNullTag)), merge, {b.build_iconst_i32(0)}, classify, {});
    // What is left of the inline answers: a Number or an Int32 (no Boolean,
    // Object or nullish value comes this far).
    b.position_at_end(classify);
    Value* is_num = b.build_ule(v, bits(kBronzeNumberMaxBits));
    // |x| in [1, +Inf] bits: neither a zero nor a NaN.
    Value* abs = b.build_and(v, bits(0x7FFFFFFFFFFFFFFFULL));
    Value* num_true = b.build_ult(b.build_sub(abs, bits(1)), bits(0x7FF0000000000000ULL));
    Value* is_int = b.build_ult(b.build_sub(v, bits(uint64_t{BRONZE_ABI_TAG_INT32} << BRONZE_ABI_VALUE_TAG_SHIFT)),
                                bits(uint64_t{1} << BRONZE_ABI_VALUE_TAG_SHIFT));
    Value* int_true = b.build_ne(b.build_and(v, bits(0xFFFFFFFFULL)), bits(0));
    Value* answer = b.build_select(is_num, num_true, int_true);
    b.build_br_if(b.build_or(is_num, is_int), merge, {answer}, slow, {});

    b.position_at_end(slow);
    b.build_br(merge, {b.build_and(b.build_call("bronze_unbox_bool", Type::i32(), {v}), b.build_iconst_i32(1))});
    b.position_at_end(merge);
    return result;
}

// `==` of two Numbers is the ordered double compare (NaN == NaN is false,
// +0 == -0 true); anything else is the helper's, out of line.
Value* lower_hinted_loose_eq(Builder& b, Value* op0, Value* op1) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "leq_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(prefix + "_num");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i32());
    slow->set_cold();

    b.position_at_end(cur);
    Value* max = b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits));
    b.build_br_if(b.build_and(b.build_ule(op0, max), b.build_ule(op1, max)), fast, slow);

    b.position_at_end(slow);
    b.build_br(merge, {b.build_and(b.build_call("bronze_loose_eq", Type::i32(), {op0, op1}),
                                   b.build_iconst_i32(1))});

    b.position_at_end(fast);
    b.build_br(merge, {b.build_eq(b.build_bitcast_f64_i64(op0), b.build_bitcast_f64_i64(op1))});

    b.position_at_end(merge);
    return result;
}

// Unary `-` of a Number is its double negated (NaN canonicalized on the box);
// a BigInt, or anything ToNumeric must convert, is the helper's.
Value* lower_hinted_neg(Builder& b, Value* op0) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "neg_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(prefix + "_num");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());
    slow->set_cold();

    b.position_at_end(cur);
    b.build_br_if(b.build_ule(op0, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits))), fast, slow);

    b.position_at_end(slow);
    b.build_br(merge, {b.build_call("bronze_dynamic_neg", Type::i64(), {op0})});

    b.position_at_end(fast);
    b.build_br(merge, {box_f64_bits(b, b.build_neg(b.build_bitcast_f64_i64(op0)))});

    b.position_at_end(merge);
    return result;
}

} // namespace il2mir
