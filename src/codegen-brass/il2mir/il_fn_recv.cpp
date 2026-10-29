// A read whose receiver expression names a class (il.h `icFnRecv`):
// `Object3D.DEFAULT_UP`, `Euler.DEFAULT_ORDER` in a default parameter,
// `Vector3.prototype` in `Vector3.prototype.isVector3 = true`. A function has
// no shape word, so the plain arm's guard can only miss on one, and every such
// read went to the helper, which walks rt_prop_function.cpp's whole ladder.
// Two arms ask the receiver's flags first and answer as that ladder does:
//
//  - `prototype` of a CLASS constructor is its header's prototype slot: a
//    class is never an intrinsic constructor (whose `prototype` the
//    global-constructor table answers, ahead of the slot), always has the
//    property (15.7.14 makes it, non-writable and non-configurable, so no
//    write or define can move it), and the slot holds it from the class's
//    creation; an empty slot still takes the helper.
//  - any other key of a CLASS constructor is an own data property of its
//    statics box when the box's shape is the one way 0 names with an own-data
//    slot word: the ladder reads the box's own properties first for anything
//    but `prototype` (and the box's shape is what the helper fills the entry
//    with, installStaticsCacheEntry), and a shape is a key-to-slot map, so a
//    box at that shape holds the key at that slot. A class is never an
//    intrinsic constructor, so the table that answers those ahead of their
//    box never applies.
//
// A receiver that is no function takes the site's ordinary read.

#include "il_abi.h"
#include "il_property.h"
#include <brass/mir/function.hpp>

namespace il2mir {

namespace {
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr int32_t kFunctionHeaderLow = (BRONZE_ABI_OBJ_FLAGS_FUNCTION << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int64_t kIcOwnDataLimit = int64_t{1} << 32;
} // namespace

Value* PropertyLoweringHelper::lower_prop_get_fn_recv(Builder& b, Value* obj, uint32_t key_index, const IcSite& site,
                                                      const std::function<Value*(Builder&)>& ordinary) {
    Function* fn = b.current_block()->parent();
    const bool proto = key_tag(key_index) == "prototype";
    const std::string prefix = "fnrecv_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_entry = b.current_block();
    BasicBlock* bb_fn = b.append_block(prefix + "_fn");
    BasicBlock* bb_hit = b.append_block(prefix + "_hit");
    BasicBlock* bb_helper = b.append_block(prefix + "_helper");
    BasicBlock* bb_other = b.append_block(prefix + "_other");
    BasicBlock* bb_done = b.append_block(prefix + "_done");
    BasicBlock* bb_head = b.append_block(prefix + "_head");
    BasicBlock* bb_box = proto ? nullptr : b.append_block(prefix + "_box");
    BasicBlock* bb_load = proto ? nullptr : b.append_block(prefix + "_load");
    Value* result = b.add_block_param(bb_done, Type::i64());

    // An object, then a function's header word, then a class constructor's
    // flag bit.
    b.position_at_end(bb_entry);
    Value* ptr = b.build_xor(obj, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    Value* is_obj = b.build_ult(ptr, b.build_iconst_i64(kPayloadLimit));
    b.position_at_end(bb_entry);
    b.build_br_if(is_obj, bb_head, bb_other);
    b.position_at_end(bb_head);
    Value* is_fn = b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kFunctionHeaderLow));
    b.build_br_if(is_fn, bb_fn, bb_other);

    b.position_at_end(bb_fn);
    Value* flags = b.build_load(Type::i64(), ptr, BRONZE_ABI_FN_FLAGS_WORD_OFFSET);
    Value* is_class =
        b.build_ne(b.build_and(flags, b.build_iconst_i64(BRONZE_ABI_FN_FLAG_CLASS_CTOR)), b.build_iconst_i64(0));
    Value* answer = nullptr;
    if (proto) {
        answer = b.build_load(Type::i64(), ptr, BRONZE_ABI_FN_PROTOTYPE_OFFSET);
        Value* is_object = b.build_ult(b.build_xor(answer, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits))),
                                       b.build_iconst_i64(kPayloadLimit));
        b.build_br_if(b.build_and(is_class, is_object), bb_hit, bb_helper);
    } else {
        Value* box = b.build_load(Type::i64(), ptr, BRONZE_ABI_FN_PROPERTIES_OFFSET);
        Value* box_ptr = b.build_xor(box, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
        Value* box_obj = b.build_ult(box_ptr, b.build_iconst_i64(kPayloadLimit));
        b.build_br_if(b.build_and(is_class, box_obj), bb_box, bb_helper);
        b.position_at_end(bb_box);
        Value* plain = b.build_eq(b.build_load(Type::i32(), box_ptr, 0), b.build_iconst_i32(kPlainHeaderLow));
        Value* shape = b.build_load(Type::i64(), box_ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
        Value* cached = b.build_load(Type::i64(), site.base, site.off + BRONZE_ABI_IC_SHAPE_OFFSET);
        Value* slot_word = b.build_load(Type::i64(), site.base, site.off + BRONZE_ABI_IC_SLOT_OFFSET);
        Value* own_data = b.build_ult(slot_word, b.build_iconst_i64(kIcOwnDataLimit));
        b.build_br_if(b.build_and(plain, b.build_and(b.build_eq(shape, cached), own_data)), bb_load, bb_helper);
        b.position_at_end(bb_load);
        answer = load_slot(b, box_ptr, slot_word);
        b.build_br(bb_hit);
    }

    b.position_at_end(bb_hit);
    b.build_br(bb_done, {answer});

    b.position_at_end(bb_helper);
    Value* slow = lower_prop_get(b, obj, key_index, site_ptr(b, site));
    b.build_br(bb_done, {slow});

    b.position_at_end(bb_other);
    Value* other = ordinary(b);
    b.build_br(bb_done, {other});

    b.position_at_end(bb_done);
    return result;
}

} // namespace il2mir
