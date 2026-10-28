// The inline Array arms (il_array_method.h). Each is the runtime native's own
// fast path, test for test (builtin_array_mutators.cpp), so the arm and the
// native it skips can never disagree about which arrays they answer for:
//
//   push   an Array with no named-property box and room in its block past
//          its last element (head + length < capacity): the value stored
//          there, `length` one more, and the new length returned.
//   shift  an Array with no box and at least one element, while the TLS hole
//          latch says `Array.prototype`'s chain holds no index: the first
//          element (a hole reads as undefined) taken out, a hole left in its
//          place, the head moved past it, `length` one less, and both reset to
//          the block's start when the array empties.
//
// The box test is what answers for every array these must not touch. A frozen,
// sealed or non-extensible array has one (its integrity level lives on it),
// and so does an array with an own named property, with a custom prototype
// (whose elements a hole would read through), or a subclass instance; each
// takes the site's call, which reaches the native's full algorithm. No
// element-level attribute exists without a box, which is also why the stored
// slot needs no writability test.

#include "il_array_method.h"

#include "il_abi.h"
#include "il_lowering.h"
#include "il_lowering_direct.h"
#include "il_property.h"
#include "il/il.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <string>

namespace il2mir {

namespace {

constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr uint64_t kTagMask = 0xFFFF000000000000ULL;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;

Value* iconst(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

Value* tls_block(Builder& b) {
    Module* mod = b.current_block()->parent()->parent();
    return (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                               : b.build_call("bronze_tls_block_addr", Type::i64(), {});
}

// A count as a Number's bits: an exact integer below 2^32, so no NaN to
// canonicalize.
Value* box_count(Builder& b, Value* n) { return b.build_bitcast_i64_f64(b.build_sitofp_f64_i64(n)); }

}  // namespace

ArrayInline array_method_inline(IlLowering* lowering, const BronzeInstruction& inst) {
    if (!lowering || (inst.imm_i64 & bronze::il::Instruction::kMethodRecvArray) == 0) return ArrayInline::None;
    if (!direct_method_ic_here(lowering) || lowering->run_once()) return ArrayInline::None;
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    if (!pl.enable_inlined_fastpaths()) return ArrayInline::None;
    const std::string key(pl.key_tag(inst.index));
    if (key == "push" && inst.param_count == 1) return ArrayInline::Push;
    if (key == "shift" && inst.param_count == 0) return ArrayInline::Shift;
    return ArrayInline::None;
}

Value* lower_array_method_inline(IlLowering* lowering, Builder& b, ArrayInline which, Value* recv, Value* val,
                                 const std::function<Value*(Builder&)>& slow) {
    (void)lowering;
    BasicBlock* cur = b.current_block();
    const std::string prefix = std::string(which == ArrayInline::Push ? "arr_push_" : "arr_shift_") +
                               std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_arr = b.append_block(prefix + "_array");
    BasicBlock* bb_fast = b.append_block(prefix + "_fast");
    BasicBlock* bb_slow = b.append_block(prefix + "_call");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i64());

    // An Array: the header is read through the TLS block (mapped, and its
    // first word no header: bronze_abi.h, proto_epoch) when the value is no
    // object at all, so the header compare alone refuses it.
    b.position_at_end(cur);
    Value* ptr = b.build_xor(recv, iconst(b, kObjectTagBits));
    Value* is_obj = b.build_ult(ptr, b.build_iconst_i64(kPayloadLimit));
    Value* base = b.build_select(is_obj, ptr, tls_block(b));
    Value* is_arr = b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kArrayHeaderLow));
    b.build_br_if(is_arr, bb_arr, bb_slow);

    b.position_at_end(bb_arr);
    Value* length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
    Value* head = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET));
    Value* props = b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
    Value* no_props = b.build_ne(b.build_and(props, iconst(b, kTagMask)), iconst(b, kObjectTagBits));
    Value* pos = nullptr;
    if (which == ArrayInline::Push) {
        Value* capacity = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET));
        pos = b.build_add(head, length);
        b.build_br_if(b.build_and(no_props, b.build_ult(pos, capacity)), bb_fast, bb_slow);
    } else {
        // And the hole latch (bronze_abi_tls.h, array_hole_epoch): the block can
        // be moved down as it stands only while no hole could read through to
        // `Array.prototype`'s chain, which the native's fast path asks too
        // (rtArrayHolesSeeChain). The latch is the chain's half of that; a
        // miss reaches the native, which re-latches or walks.
        Value* tls = tls_block(b);
        Value* hole_epoch = b.build_load(Type::i64(), tls, BRONZE_TLS_ARRAY_HOLE_EPOCH_OFF);
        Value* epoch = b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF);
        Value* holes_clean = b.build_eq(hole_epoch, epoch);
        b.build_br_if(b.build_and(b.build_and(no_props, holes_clean), b.build_ne(length, iconst(b, 0))),
                      bb_fast, bb_slow);
    }

    b.position_at_end(bb_fast);
    Value* block = b.build_and(b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET), iconst(b, kPayloadMask));
    if (which == ArrayInline::Push) {
        // The slot past the last element is a hole (the block is hole-filled
        // past `length`), so the store is an append and nothing is overwritten.
        b.build_store_indexed(Type::i64(), block, pos, 8, static_cast<int32_t>(kBronzeHdrBytes), val);
        Value* new_len = b.build_add(length, iconst(b, 1));
        b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET, b.build_trunc_i32(new_len));
        Value* boxed_len = box_count(b, new_len);
        // The block is a heap object like any other, so a reference stored in
        // it takes the barrier every element store does (every reference is
        // tagged above the Number range).
        BasicBlock* bb_barrier = b.append_block(prefix + "_barrier");
        BasicBlock* bb_stored = b.append_block(prefix + "_stored");
        b.position_at_end(bb_fast);
        b.build_br_if(b.build_ugt(val, iconst(b, kBronzeNumberMaxBits)), bb_barrier, bb_stored);
        b.position_at_end(bb_barrier);
        Value* slot_addr = b.build_add(b.build_add(block, b.build_shl(pos, iconst(b, 3))), iconst(b, kBronzeHdrBytes));
        b.build_call("brass_gc_write_barrier", Type::void_type(), {slot_addr, val});
        b.build_br(bb_stored);
        b.position_at_end(bb_stored);
        b.build_br(bb_merge, {boxed_len});
    } else {
        Value* first = b.build_load_indexed(Type::i64(), block, head, 8, static_cast<int32_t>(kBronzeHdrBytes));
        // A hole stores no reference, so it needs no barrier.
        b.build_store_indexed(Type::i64(), block, head, 8, static_cast<int32_t>(kBronzeHdrBytes),
                              iconst(b, BRONZE_ABI_HOLE_BITS));
        Value* new_len = b.build_sub(length, iconst(b, 1));
        Value* empty = b.build_eq(new_len, iconst(b, 0));
        Value* new_head = b.build_select(empty, iconst(b, 0), b.build_add(head, iconst(b, 1)));
        b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET, b.build_trunc_i32(new_len));
        b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET, b.build_trunc_i32(new_head));
        Value* is_hole = b.build_eq(first, iconst(b, BRONZE_ABI_HOLE_BITS));
        b.build_br(bb_merge, {b.build_select(is_hole, iconst(b, kUndefinedTag), first)});
    }

    b.position_at_end(bb_slow);
    b.build_br(bb_merge, {slow(b)});

    b.position_at_end(bb_merge);
    return result;
}

}  // namespace il2mir
