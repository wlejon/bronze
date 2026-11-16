// The dynamic (NaN-boxed) forms of `===` and ToBoolean: an inline arm for
// the operands whose answer is in their bits, and the runtime helper for the
// rest. The numeric ops' forms are il_lowering_numeric.cpp.

#include "il_lowering_dynamic.h"
#include "il_lowering.h"
#include "il_lowering_ops.h"
#include "il_abi.h"
#include <unordered_map>
#include <vector>

namespace il2mir {

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
// compare: no Number, String or BigInt has those bits. Otherwise equal bits
// are asked first, and are equal unless they are a Number that is NaN (the
// one value not === itself): `x.k !== k` over an unchanged value — the same
// object, string, Boolean or Number — is that compare and one range test.
// Different bits are two Numbers compared as doubles (+0 === -0), and only
// two Strings or two BigInts need the helper's contents compare; any other
// pair with different bits is false.
Value* lower_dynamic_strict_eq(Builder& b, Value* op0, Value* op1, PropertyLoweringHelper* arms) {
    if (is_bits_identity_constant(op0) || is_bits_identity_constant(op1)) return b.build_eq(op0, op1);
    BasicBlock* cur = b.current_block();
    const std::string prefix = "seq_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* same = b.append_block(prefix + "_same");
    BasicBlock* same_num = b.append_block(prefix + "_samenum");
    BasicBlock* differ = b.append_block(prefix + "_differ");
    BasicBlock* num = b.append_block(prefix + "_num");
    BasicBlock* other = b.append_block(prefix + "_other");
    BasicBlock* contents = b.append_block(prefix + "_contents");
    BasicBlock* strings = b.append_block(prefix + "_strings");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i32());

    // The compare's ARMS (il_property.h arm_begin): equal bits of a Number, of a
    // String or BigInt, or of anything else; two Numbers with different
    // bits; and different bits whose first operand is or is not a String or
    // a BigInt.
    enum : uint32_t { kArmSame = 1, kArmSameNumber, kArmNumbers, kArmIdentity, kArmText, kArmSameText };
    const uint64_t text_bits = uint64_t{BRONZE_ABI_TAG_STRING} << BRONZE_ABI_VALUE_TAG_SHIFT;
    auto bits = [&](uint64_t x) { return b.build_iconst_i64(static_cast<int64_t>(x)); };
    // A block that marks `arm` and joins with `answer`.
    auto arm_edge = [&](uint32_t arm, const char* name, int32_t answer) {
        BasicBlock* at = b.current_block();
        BasicBlock* bb = b.append_block(prefix + name);
        b.position_at_end(bb);
        if (arms) arms->arm_begin(b, arm);
        b.build_br(merge, {b.build_iconst_i32(answer)});
        b.position_at_end(at);
        return bb;
    };

    b.position_at_end(cur);
    Value* max = b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits));
    b.build_br_if(b.build_eq(op0, op1), same, differ);

    // Equal bits: equal, unless a Number that is NaN.
    b.position_at_end(same);
    const bool marked = arms && arms->arms_marked();
    BasicBlock* same_other = marked ? b.append_block(prefix + "_same_other") : merge;
    b.position_at_end(same);
    if (marked) {
        b.build_br_if(b.build_ule(op0, max), same_num, same_other);
        b.position_at_end(same_other);
        b.build_br_if(b.build_ult(op0, bits(text_bits)), arm_edge(kArmSame, "_same_id", 1),
                      arm_edge(kArmSameText, "_same_text", 1));
    } else {
        b.build_br_if(b.build_ule(op0, max), same_num, {}, merge, {b.build_iconst_i32(1)});
    }
    b.position_at_end(same_num);
    if (arms) arms->arm_begin(b, kArmSameNumber);
    Value* self = b.build_bitcast_f64_i64(op0);
    b.build_br(merge, {b.build_eq(self, self)});

    b.position_at_end(differ);
    b.build_br_if(b.build_and(b.build_ule(op0, max), b.build_ule(op1, max)), num, other);

    b.position_at_end(num);
    if (arms) arms->arm_begin(b, kArmNumbers);
    b.build_br(merge, {b.build_eq(b.build_bitcast_f64_i64(op0), b.build_bitcast_f64_i64(op1))});

    // Different bits, not two Numbers: false unless two Strings or two
    // BigInts, whose contents decide. An object first operand, the tag right
    // above the Numbers, is one compare; the tags are read only past it, and
    // the second operand's only for a String or a BigInt first one.
    b.position_at_end(other);
    b.build_br_if(b.build_ult(op0, bits(text_bits)), arm_edge(kArmIdentity, "_identity", 0), contents);
    b.position_at_end(contents);
    if (arms) arms->arm_begin(b, kArmText);
    Value* shift = b.build_iconst_i64(BRONZE_ABI_VALUE_TAG_SHIFT);
    Value* tag0 = b.build_lshr(op0, shift);
    Value* by_contents = b.build_or(b.build_eq(tag0, b.build_iconst_i64(BRONZE_ABI_TAG_STRING)),
                                    b.build_eq(tag0, b.build_iconst_i64(BRONZE_ABI_TAG_BIGINT)));
    b.build_br_if(b.build_and(by_contents, b.build_eq(tag0, b.build_lshr(op1, shift))), strings, {}, merge,
                  {b.build_iconst_i32(0)});

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
        Value* texts_differ = build_strings_differ(b, s0, s1);
        Value* differ_known = b.build_and(is_string, texts_differ);
        if (arms) {
            arms->slow_branch(b, differ_known, merge, slow, {b.build_iconst_i32(0)});
        } else {
            b.build_br_if(differ_known, merge, {b.build_iconst_i32(0)}, slow, {});
        }
    }

    b.position_at_end(slow);
    if (arms) arms->slow_arm(b);
    b.build_br(merge, {b.build_and(b.build_call("bronze_strict_eq", Type::i32(), {op0, op1}),
                                   b.build_iconst_i32(1))});

    b.position_at_end(merge);
    return result;
}

// ToBoolean of a dynamic value, bronze_truthy without the call for every
// operand whose answer is in its bits: a Number is true unless it is +-0 or
// NaN, an Int32 or Boolean unless its payload is 0, undefined and null are
// false and an Object is true (bronze has no [[IsHTMLDDA]] object), and a
// String is true unless its header's length is 0. BigInts and the hole are
// the helper's.
//
// The tests are a ladder, each its own branch: a Boolean, null or undefined
// (one test), then an Object, then the Numbers and Int32s, then a String,
// then the helper.
Value* lower_dynamic_truthy(Builder& b, Value* v, PropertyLoweringHelper* arms) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "truthy_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i32());
    BasicBlock* classify = b.append_block(prefix + "_classify");
    BasicBlock* not_obj = b.append_block(prefix + "_notobj");
    BasicBlock* not_num = b.append_block(prefix + "_notnum");
    BasicBlock* str = b.append_block(prefix + "_str");
    b.position_at_end(cur);

    auto bits = [&](uint64_t x) { return b.build_iconst_i64(static_cast<int64_t>(x)); };
    // The Booleans and the two nullish values in one test: most dynamic
    // conditions are a flag field (`if (this.matrixAutoUpdate)`) or a presence
    // test of a field that is unset (`if (material.map)`, `a && b`). Their
    // tags are consecutive and their payloads 0 or 1, so `v - kBoolTag` is at
    // most undefined's distance for exactly false, true, null and undefined,
    // and only `true` among them is truthy.
    static_assert(BRONZE_ABI_TAG_NULL == BRONZE_ABI_TAG_BOOL + 1 && BRONZE_ABI_TAG_UNDEFINED == BRONZE_ABI_TAG_BOOL + 2 &&
                      kUndefinedTag == uint64_t{BRONZE_ABI_TAG_UNDEFINED} << BRONZE_ABI_VALUE_TAG_SHIFT,
                  "Boolean, null and undefined are one range of bits");
    // Each answer is an ARM (il_property.h arm_begin), marked in its own block in a
    // build that may be harvested.
    enum : uint32_t { kArmFlag = 1, kArmObject, kArmNumber, kArmString };
    const bool marked = arms != nullptr && arms->arms_marked();
    // The join with `answer`, through a block that marks `arm` when marked.
    auto answer_edge = [&](uint32_t arm, const char* name, Value* test, Value* answer, BasicBlock* miss) {
        if (!marked) {
            b.build_br_if(test, merge, {answer}, miss, {});
            return;
        }
        BasicBlock* at = b.current_block();
        BasicBlock* bb = b.append_block(prefix + name);
        b.position_at_end(bb);
        arms->arm_begin(b, arm);
        b.build_br(merge, {answer});
        b.position_at_end(at);
        b.build_br_if(test, bb, miss);
    };
    Value* is_flag = b.build_ule(b.build_sub(v, bits(kBoolTag)), bits(kUndefinedTag - kBoolTag));
    answer_edge(kArmFlag, "_flag", is_flag, b.build_eq(v, bits(kBoolTag | 1)), not_obj);
    // Then an Object: a presence test of a field that is set.
    b.position_at_end(not_obj);
    Value* is_object = b.build_eq(b.build_lshr(v, bits(BRONZE_ABI_VALUE_TAG_SHIFT)), bits(BRONZE_ABI_TAG_OBJECT));
    answer_edge(kArmObject, "_object", is_object, b.build_iconst_i32(1), classify);
    // What is left of the inline answers: a Number or an Int32 (no Boolean,
    // Object or nullish value comes this far). |x| in [1, +Inf] bits is
    // neither a zero nor a NaN.
    b.position_at_end(classify);
    {
        Value* is_num = b.build_ule(v, bits(kBronzeNumberMaxBits));
        Value* abs = b.build_and(v, bits(0x7FFFFFFFFFFFFFFFULL));
        Value* num_true = b.build_ult(b.build_sub(abs, bits(1)), bits(0x7FF0000000000000ULL));
        Value* is_int =
            b.build_ult(b.build_sub(v, bits(uint64_t{BRONZE_ABI_TAG_INT32} << BRONZE_ABI_VALUE_TAG_SHIFT)),
                        bits(uint64_t{1} << BRONZE_ABI_VALUE_TAG_SHIFT));
        Value* int_true = b.build_ne(b.build_and(v, bits(0xFFFFFFFFULL)), bits(0));
        answer_edge(kArmNumber, "_number", b.build_or(is_num, is_int), b.build_select(is_num, num_true, int_true),
                    not_num);
    }
    // Then a String (`if (renderable.renderPipeId)`, `if (name)`): true
    // unless it is empty, and every string's header holds its length, a
    // `+`-chain builder's included (string.h). The helper is the slow arm.
    b.position_at_end(not_num);
    Value* is_str = b.build_eq(b.build_lshr(v, bits(BRONZE_ABI_VALUE_TAG_SHIFT)), bits(BRONZE_ABI_TAG_STRING));
    if (arms) {
        arms->slow_branch(b, is_str, str, slow);
    } else {
        b.build_br_if(is_str, str, slow);
    }
    b.position_at_end(str);
    if (marked) arms->arm_begin(b, kArmString);
    Value* chars = b.build_load(Type::i32(), b.build_and(v, bits(0x0000FFFFFFFFFFFFULL)), BRONZE_ABI_STRING_LENGTH_OFFSET);
    b.build_br(merge, {b.build_ne(chars, b.build_iconst_i32(0))});

    b.position_at_end(slow);
    if (arms) arms->slow_arm(b);
    b.build_br(merge, {b.build_and(b.build_call("bronze_unbox_bool", Type::i32(), {v}), b.build_iconst_i32(1))});
    b.position_at_end(merge);
    return result;
}

} // namespace il2mir
