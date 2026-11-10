// `for (x of arr)` over an Array, inline: the iteration record's step and value
// (runtime/iterator.h, laid out in bronze_abi_tls.h "the iteration record"),
// with bronze_iter_step for every record that is not an ARRAY walk at an
// in-bounds, present element.
//
// What an ARRAY record already settled. `bronze_iter_open` gives a record the
// Array kind only when the source is an Array whose iteration is the
// intrinsic one (its @@iterator and %ArrayIteratorPrototype%.next unmodified),
// which is what makes a step a read of `target[cursor]` and nothing else
// (23.1.5.2.1, minus the result object). The step re-reads the length every
// time, as %ArrayIteratorPrototype%.next does, so a loop body that grows or
// shrinks the array is seen. A HOLE is the helper's: it reads through the
// prototype chain, which can run a getter.

#include "il_abi.h"
#include "il_iter_lowering.h"
#include "il_lowering.h"
#include <brass/mir/function.hpp>
#include <string>

namespace il2mir {

namespace {

constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr uint64_t kFalseBits = kBoolTag;

Value* iconst(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

}  // namespace

Value* lower_iter_step_inline(Builder& b, Value* iter) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "iterstep_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_array = b.append_block(prefix + "_array");
    BasicBlock* bb_load = b.append_block(prefix + "_load");
    BasicBlock* bb_store = b.append_block(prefix + "_store");
    BasicBlock* bb_barrier = b.append_block(prefix + "_barrier");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow_cold");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i32());

    // The operand is an iteration record (the IL's iter.open made it), so its
    // payload is read without a tag test.
    b.position_at_end(cur);
    Value* rec = b.build_and(iter, iconst(b, kPayloadMask));
    Value* is_array = b.build_eq(b.build_load(Type::i64(), rec, BRONZE_ABI_ITER_KIND_OFFSET),
                                 iconst(b, BRONZE_ABI_ITER_KIND_ARRAY_BITS));
    Value* not_done = b.build_eq(b.build_load(Type::i64(), rec, BRONZE_ABI_ITER_DONE_OFFSET), iconst(b, kFalseBits));
    b.build_br_if(b.build_and(is_array, not_done), bb_array, bb_slow);

    // The cursor is a Number the runtime keeps integral, below 2^32.
    b.position_at_end(bb_array);
    Value* arr = b.build_and(b.build_load(Type::i64(), rec, BRONZE_ABI_ITER_TARGET_OFFSET), iconst(b, kPayloadMask));
    Value* cursor = b.build_load(Type::f64(), rec, BRONZE_ABI_ITER_CURSOR_OFFSET);
    Value* i = b.build_fptosi_i64(cursor);
    Value* length = b.build_zext_i64(b.build_load(Type::i32(), arr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
    b.build_br_if(b.build_ult(i, length), bb_load, bb_slow);

    b.position_at_end(bb_load);
    Value* block = b.build_and(b.build_load(Type::i64(), arr, BRONZE_ABI_ARRAY_ELEMS_OFFSET), iconst(b, kPayloadMask));
    Value* head = b.build_zext_i64(b.build_load(Type::i32(), arr, BRONZE_ABI_ARRAY_HEAD_OFFSET));
    Value* elem = b.build_load_indexed(Type::i64(), block, b.build_add(i, head), 8,
                                       static_cast<int32_t>(kBronzeHdrBytes));
    b.build_br_if(b.build_eq(elem, iconst(b, BRONZE_ABI_HOLE_BITS)), bb_slow, bb_store);

    // `current` is a heap slot of the record, so a reference stored there
    // passes the write barrier; the cursor is a Number and needs none.
    b.position_at_end(bb_store);
    b.build_store(Type::f64(), rec, BRONZE_ABI_ITER_CURSOR_OFFSET,
                  b.build_sitofp_f64_i64(b.build_add(i, iconst(b, 1))));
    b.build_store(Type::i64(), rec, BRONZE_ABI_ITER_CURRENT_OFFSET, elem);
    Value* one = b.build_iconst_i32(1);
    b.build_br_if(b.build_ugt(elem, iconst(b, kBronzeNumberMaxBits)), bb_barrier, {}, bb_merge, {one});

    b.position_at_end(bb_barrier);
    b.build_write_barrier(b.build_add(rec, iconst(b, BRONZE_ABI_ITER_CURRENT_OFFSET)), elem);
    b.build_br(bb_merge, {b.build_iconst_i32(1)});

    b.position_at_end(bb_slow);
    b.build_br(bb_merge, {b.build_and(b.build_call("bronze_iter_step", Type::i32(), {iter}), b.build_iconst_i32(1))});

    b.position_at_end(bb_merge);
    return result;
}

Value* lower_iter_value_inline(Builder& b, Value* iter) {
    // What the last step produced, for every kind: bronze_iter_value is this
    // load.
    return b.build_load(Type::i64(), b.build_and(iter, iconst(b, kPayloadMask)), BRONZE_ABI_ITER_CURRENT_OFFSET);
}

void lower_iter_close_inline(Builder& b, Value* iter, int64_t suppress_imm) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "iterclose_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_slow = b.append_block(prefix + "_slow_cold");
    BasicBlock* bb_done = b.append_block(prefix + "_done");

    b.position_at_end(cur);
    Value* rec = b.build_and(iter, iconst(b, kPayloadMask));
    Value* kind_bits = b.build_load(Type::i64(), rec, BRONZE_ABI_ITER_KIND_OFFSET);
    Value* is_owned = b.build_ult(kind_bits, iconst(b, BRONZE_ABI_ITER_KIND_OWNED_LIMIT_BITS));
    Value* done_bits = b.build_load(Type::i64(), rec, BRONZE_ABI_ITER_DONE_OFFSET);
    Value* is_done = b.build_ne(done_bits, iconst(b, kFalseBits));
    b.build_br_if(b.build_or(is_owned, is_done), bb_done, bb_slow);

    b.position_at_end(bb_slow);
    Value* suppress = b.build_iconst_i32(suppress_imm != 0 ? 1 : 0);
    b.build_call("bronze_iter_close", Type::void_type(), {iter, suppress});
    b.build_br(bb_done);

    b.position_at_end(bb_done);
}

}  // namespace il2mir
