// A `define.own.attr` site's inline add (il.h `defineAddCell`): the add the
// helper last made at this site, repeated for a receiver at the same shape.
//
// three.js gives every Object3D seven literal-descriptor defines (`id`, then
// `position` ... `normalMatrix`), and each one through the helper asks the
// receiver's kind, the descriptor-field pollution memo, looks the key up on the
// shape, and then takes setProp's add with its own lookup and transition scan —
// for an answer that is the same edge of the same transition tree on every
// construction. The cells hold that edge's child, END, and the arm asks only
// what makes repeating it the same define:
//
//  - a plain object with no internal slots (the full header word, as the
//    assignment's transition arm asks), so no array, function statics box,
//    typed array, RegExp, proxy or String wrapper — each of which the helper
//    sends elsewhere — and no non-object, whose TypeError is the helper's;
//  - END's parent is the receiver's shape: the key is absent (keys are unique
//    along a chain, and END's own key is this site's, which the helper
//    verified), the receiver is extensible (a non-extensible object is a
//    dictionary, and no tree node is a dictionary's child), and the new
//    property's attributes are END's, which the helper verified against the
//    mask. [[DefineOwnProperty]] on an ordinary object never consults the
//    prototype chain, so nothing else about the receiver matters;
//  - the receiver's shape is no prototype's (an add to one bumps the epoch,
//    which is the helper's job) and not a dictionary, asked directly;
//  - the epoch the cells were filled at: no descriptor field name has appeared
//    on Object.prototype since the helper found it had none, so the literal
//    still describes exactly the fields it wrote (builtin_object_define.cpp);
//  - a value fit for END's slot (a Number, where END made the slot a double);
//  - room for the slot without allocating: inline, or an overflow block
//    already big enough, read as the transition arm reads it.

#include "il_abi.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>

namespace il2mir {

namespace {
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr int32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int64_t kBarePlainHeader =
    (int64_t{BRONZE_ABI_PLAIN_OBJECT_BYTES} << (8 * BRONZE_ABI_HDR_SIZE_OFFSET)) | kPlainHeaderLow;
constexpr int32_t kCell = static_cast<int32_t>(sizeof(uint64_t));
} // namespace

void PropertyLoweringHelper::lower_define_own_attr(Builder& b, Value* obj, Value* key_id, Value* val, uint32_t mask,
                                                   uint32_t cell, bool arm, bool val_is_number) {
    const bool cached = cell != kNoDefineAddCell && static_cells_cover(cell, BRONZE_ABI_DEFINE_ADD_CELLS);
    auto cells_at = [&]() -> Value* {
        return b.build_add(static_cells_addr(b), b.build_iconst_i64(static_cast<int64_t>(uint64_t{cell} * kCell)));
    };
    auto helper = [&] {
        Value* cells = cached ? cells_at() : b.build_iconst_i64(0);
        b.build_call("bronze_define_own_attr", Type::void_type(),
                     {obj, key_id, val, b.build_iconst_i32(static_cast<int32_t>(mask)), cells});
    };
    if (!cached || !arm) {
        helper();
        return;
    }

    Function* fn = b.current_block()->parent();
    const std::string prefix = "defadd_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_entry = b.current_block();
    BasicBlock* bb_check = b.append_block(prefix + "_check");
    BasicBlock* bb_room = b.append_block(prefix + "_room");
    BasicBlock* bb_store = b.append_block(prefix + "_store");
    BasicBlock* bb_helper = b.append_block(prefix + "_helper_cold");
    BasicBlock* bb_done = b.append_block(prefix + "_done");

    b.position_at_end(bb_entry);
    Value* plain = nullptr;
    Value* ptr = nullptr;
    Value* shape = nullptr;
    Value* base = nullptr;
    receiver_shape(b, obj, plain, ptr, shape, &base);
    Value* cells = cells_at();
    Value* end = b.build_load(Type::i64(), cells, BRONZE_ABI_DEFINE_ADD_END * kCell);
    Value* bare = b.build_eq(b.build_load(Type::i64(), base, 0), b.build_iconst_i64(kBarePlainHeader));
    // Zero is an unfilled site: no shape is at address zero.
    Value* filled = b.build_ne(end, b.build_iconst_i64(0));
    b.build_br_if(b.build_and(bare, filled), bb_check, bb_helper);

    b.position_at_end(bb_check);
    Value* is_child = b.build_eq(b.build_load(Type::i64(), end, BRONZE_ABI_SHAPE_PARENT_OFFSET), shape);
    Value* not_dict =
        b.build_eq(b.build_load(Type::i64(), shape, BRONZE_ABI_SHAPE_DICT_OFFSET), b.build_iconst_i64(0));
    Value* not_proto = b.build_eq(b.build_zext_i64(b.build_load(Type::i8(), shape, BRONZE_ABI_SHAPE_USEDPROTO_OFFSET)),
                                  b.build_iconst_i64(0));
    Module* mod = fn->parent();
    Value* tls = (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                                     : b.build_call("bronze_tls_block_addr", Type::i64(), {});
    Value* epoch_ok = b.build_eq(b.build_load(Type::i64(), cells, BRONZE_ABI_DEFINE_ADD_EPOCH * kCell),
                                 b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF));
    Value* ok = b.build_and(b.build_and(is_child, not_dict), b.build_and(not_proto, epoch_ok));
    if (!val_is_number) {
        // END's own node says whether it made the slot a double; the bits of
        // a boxed Number are the double's, so only a non-Number is refused.
        Value* boxed = b.build_eq(b.build_zext_i64(b.build_load(Type::i8(), end, BRONZE_ABI_SHAPE_REPR_OFFSET)),
                                  b.build_iconst_i64(0));
        Value* number = b.build_ule(val, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
        ok = b.build_and(ok, b.build_or(boxed, number));
    }
    b.build_br_if(ok, bb_room, bb_helper);

    b.position_at_end(bb_room);
    Value* slot = b.build_zext_i64(b.build_load(Type::i32(), end, BRONZE_ABI_SHAPE_SLOTINDEX_OFFSET));
    Value* is_inline = b.build_ult(slot, b.build_iconst_i64(kBronzeObjInlineSlots));
    Value* overflow = b.build_load(Type::i64(), ptr, kBronzeObjOverflowOffset);
    Value* block = b.build_xor(overflow, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    Value* has_block = b.build_ult(block, b.build_iconst_i64(kPayloadLimit));
    Value* block_base = b.build_select(has_block, block, ptr);
    Value* size = b.build_zext_i64(b.build_load(Type::i32(), block_base, BRONZE_ABI_HDR_SIZE_OFFSET));
    Value* capacity =
        b.build_lshr(b.build_sub(size, b.build_iconst_i64(BRONZE_ABI_HDR_BYTES)), b.build_iconst_i64(3));
    Value* fits = b.build_and(
        has_block, b.build_ult(b.build_sub(slot, b.build_iconst_i64(kBronzeObjInlineSlots)), capacity));
    b.build_br_if(b.build_or(is_inline, fits), bb_store, bb_helper);

    // The shape first, then the value (the new slot already holds undefined),
    // then the barrier: nothing between them allocates, so `ptr` holds.
    b.position_at_end(bb_store);
    b.build_store(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET, end);
    Value* slot_addr = b.build_add(b.build_add(slot_base(b, ptr, slot), b.build_shl(slot, b.build_iconst_i64(3))),
                                   b.build_iconst_i64(kBronzeObjSlotsOffset));
    store_with_barrier(b, slot_addr, 0, val, !val_is_number && !is_known_non_ref(val), prefix);
    b.build_br(bb_done);

    b.position_at_end(bb_helper);
    helper();
    b.build_br(bb_done);
    bb_helper->set_cold();

    b.position_at_end(bb_done);
}

} // namespace il2mir
