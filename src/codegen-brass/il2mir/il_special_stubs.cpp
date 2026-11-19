// Specialized site stubs (il_prop_stubs.h): `instanceof` site cells and
// bare NUMBERS array `.length = n` truncation behind way-0 property writes.

#include "il_prop_stubs.h"

#include "il_abi.h"

#include <brass/mir/block.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>

#include <string>

namespace il2mir {

namespace {

constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr int32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;

Value* imm(Builder& b, int64_t v) { return b.build_iconst_i64(v); }

Value* tls(Builder& b) {
    Module* mod = b.current_block()->parent()->parent();
    return (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                               : b.build_call("bronze_tls_block_addr", Type::i64(), {});
}

}  // namespace

// An `instanceof` site's stub (BRONZE_ABI_INSTANCEOF_CELL_*): while `ctor`
// matches the cell's instance & statics shapes and prototype epoch (covering
// JS ctors and eligible natives like TypedArray/Number/Map/Set), answers
// primitives false, Plain/TypedArray by memo shape or kMaxInstanceofWalk links,
// and bare Arrays by memoized `rtArrayShape` before falling back to the helper.
void build_instanceof_stub(Function* fn) {
    constexpr int kMaxInstanceofWalk = 8;
    constexpr int32_t kFunctionHeaderLow = (BRONZE_ABI_OBJ_FLAGS_FUNCTION << 16) | BRONZE_ABI_TAG_OBJECT;
    constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
    constexpr int32_t kTypedArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    Value* obj = b.add_block_param(entry, Type::i64());
    Value* ctor = b.add_block_param(entry, Type::i64());
    Value* cell = b.add_block_param(entry, Type::i64());
    BasicBlock* ctor_bb = b.append_block("io_ctor");
    BasicBlock* fn_bb = b.append_block("io_fn");
    BasicBlock* certified = b.append_block("io_certified");
    BasicBlock* object = b.append_block("io_object");
    BasicBlock* arr_bb = b.append_block("io_arr");
    BasicBlock* primitive = b.append_block("io_primitive");
    BasicBlock* memo = b.append_block("io_memo");
    BasicBlock* memo_hit = b.append_block("io_memo_hit");
    BasicBlock* walk = b.append_block("io_walk");
    BasicBlock* yes = b.append_block("io_yes");
    BasicBlock* no = b.append_block("io_no");
    BasicBlock* helper = b.append_block("io_helper");
    auto word_at = [&](int32_t w) { return b.build_load(Type::i64(), cell, w * 8); };
    auto plain_at = [&](Value* p) {
        return b.build_eq(b.build_load(Type::i32(), p, 0), b.build_iconst_i32(kPlainHeaderLow));
    };
    auto ta_at = [&](Value* p) {
        return b.build_eq(b.build_load(Type::i32(), p, 0), b.build_iconst_i32(kTypedArrayHeaderLow));
    };

    b.position_at_end(entry);
    Value* cptr = b.build_xor(ctor, imm(b, static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(cptr, imm(b, kPayloadLimit)), ctor_bb, helper);
    b.position_at_end(ctor_bb);
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), cptr, 0), b.build_iconst_i32(kFunctionHeaderLow)), fn_bb,
                  helper);
    b.position_at_end(fn_bb);
    Value* inst_ok = b.build_eq(b.build_load(Type::i64(), cptr, BRONZE_ABI_FN_INSTANCE_SHAPE_OFFSET),
                                word_at(BRONZE_ABI_INSTANCEOF_CELL_INSTANCE));
    Value* statics_ok = b.build_eq(b.build_load(Type::i64(), cptr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
                                   word_at(BRONZE_ABI_INSTANCEOF_CELL_STATICS));
    Value* epoch_ok = b.build_eq(word_at(BRONZE_ABI_INSTANCEOF_CELL_EPOCH),
                                 b.build_load(Type::i64(), tls(b), BRONZE_TLS_PROTO_EPOCH_OFF));
    b.build_br_if(b.build_and(inst_ok, b.build_and(statics_ok, epoch_ok)), certified, helper);

    b.position_at_end(certified);
    Value* ptr = b.build_xor(obj, imm(b, static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(ptr, imm(b, kPayloadLimit)), object, primitive);
    b.position_at_end(object);
    Value* hdr = b.build_load(Type::i32(), ptr, 0);
    Value* walkable_hdr = b.build_or(b.build_eq(hdr, b.build_iconst_i32(kPlainHeaderLow)),
                                     b.build_eq(hdr, b.build_iconst_i32(kTypedArrayHeaderLow)));
    b.build_br_if(walkable_hdr, memo, arr_bb);
    b.position_at_end(arr_bb);
    BasicBlock* arr_check = b.append_block("io_arr_check");
    b.position_at_end(arr_bb);
    b.build_br_if(b.build_eq(hdr, b.build_iconst_i32(kArrayHeaderLow)), arr_check, helper);
    b.position_at_end(arr_check);
    b.build_br_if(b.build_eq(b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
                             word_at(BRONZE_ABI_INSTANCEOF_CELL_SHAPE)),
                  memo_hit, helper);
    b.position_at_end(memo);
    Value* shape0 = b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    b.build_br_if(b.build_eq(shape0, word_at(BRONZE_ABI_INSTANCEOF_CELL_SHAPE)), memo_hit, walk);
    b.position_at_end(memo_hit);
    b.build_ret(b.build_trunc_i32(word_at(BRONZE_ABI_INSTANCEOF_CELL_ANSWER)));
    b.position_at_end(walk);
    Value* target = b.build_load(Type::i64(), cptr, BRONZE_ABI_FN_PROTOTYPE_OFFSET);
    Value* cur = ptr;
    for (int link = 1; link <= kMaxInstanceofWalk; ++link) {
        const bool last = link == kMaxInstanceofWalk;
        const std::string at = "io_link" + std::to_string(link);
        BasicBlock* here = b.current_block();
        BasicBlock* plain = b.append_block(at + "_plain");
        BasicBlock* rooted = b.append_block(at + "_root");
        BasicBlock* other = b.append_block(at + "_other");
        BasicBlock* not_obj = last ? nullptr : b.append_block(at + "_notobj");
        BasicBlock* is_obj = last ? nullptr : b.append_block(at + "_obj");
        b.position_at_end(here);
        Value* ok_link = (link == 1) ? b.build_or(plain_at(cur), ta_at(cur)) : plain_at(cur);
        b.build_br_if(ok_link, plain, helper);
        b.position_at_end(plain);
        Value* root = b.build_load(Type::i64(), b.build_load(Type::i64(), cur, BRONZE_ABI_OBJ_SHAPE_OFFSET),
                                   BRONZE_ABI_SHAPE_ROOT_OFFSET);
        b.build_br_if(b.build_ne(root, imm(b, 0)), rooted, helper);
        b.position_at_end(rooted);
        Value* proto = b.build_load(Type::i64(), root, BRONZE_ABI_SHAPE_PROTO_OFFSET);
        b.build_br_if(b.build_eq(proto, target), yes, other);
        b.position_at_end(other);
        Value* is_null = b.build_eq(proto, imm(b, static_cast<int64_t>(kNullTag)));
        if (last) {
            b.build_br_if(is_null, no, helper);
            break;
        }
        Value* pptr = b.build_xor(proto, imm(b, static_cast<int64_t>(kObjectTagBits)));
        b.build_br_if(b.build_ult(pptr, imm(b, kPayloadLimit)), is_obj, not_obj);
        b.position_at_end(not_obj);
        b.build_br_if(is_null, no, helper);
        b.position_at_end(is_obj);
        cur = pptr;
    }

    // Cache non-dictionary receiver shapes in the cell for subsequent probes.
    auto remember = [&](int64_t answer) {
        Value* is_dict = b.build_ne(b.build_load(Type::i64(), shape0, BRONZE_ABI_SHAPE_DICT_OFFSET), imm(b, 0));
        b.build_store(Type::i64(), cell, BRONZE_ABI_INSTANCEOF_CELL_SHAPE * 8, b.build_select(is_dict, imm(b, 0), shape0));
        b.build_store(Type::i64(), cell, BRONZE_ABI_INSTANCEOF_CELL_ANSWER * 8, imm(b, answer));
        b.build_ret(b.build_iconst_i32(static_cast<int32_t>(answer)));
    };
    b.position_at_end(yes);
    remember(1);
    b.position_at_end(no);
    remember(0);
    b.position_at_end(primitive);
    b.build_ret(b.build_iconst_i32(0));
    b.position_at_end(helper);
    b.build_ret(b.build_call("bronze_instanceof_cell", Type::i32(), {obj, ctor, cell}));
    fn->rebuild_cfg_predecessors();
}

// Fast path for `arr.length = n` on bare NUMBERS arrays (shrinking or keeping
// length in [0, min(length, 2^31 - 1)]): updates header length (and resets
// head_offset when zero) with no IC walk, no C++ call, and no hole-fill.
void build_prop_set_length_stub(Function* fn, const std::string& prop_set_sym) {
    constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    Value* obj = b.add_block_param(entry, Type::i64());
    Value* key = b.add_block_param(entry, Type::i32());
    Value* val = b.add_block_param(entry, Type::i64());
    Value* site = b.add_block_param(entry, Type::ptr());
    Value* mode = b.add_block_param(entry, Type::i32());
    BasicBlock* bb_obj = b.append_block("psl_obj");
    BasicBlock* bb_num = b.append_block("psl_num");
    BasicBlock* bb_trunc = b.append_block("psl_trunc");
    BasicBlock* bb_zero_head = b.append_block("psl_zero_head");
    BasicBlock* bb_done = b.append_block("psl_done");
    BasicBlock* bb_fallback = b.append_block("psl_fallback");

    b.position_at_end(entry);
    Value* ptr = b.build_xor(obj, imm(b, static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(ptr, imm(b, kPayloadLimit)), bb_obj, bb_fallback);

    b.position_at_end(bb_obj);
    Value* is_arr = b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kArrayHeaderLow));
    Value* is_bare = b.build_eq(b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
                                b.build_load(Type::i64(), tls(b), BRONZE_TLS_ARRAY_SHAPE_OFF));
    Value* bits = b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET);
    Value* is_nums = b.build_ne(b.build_and(bits, b.build_iconst_i32(BRONZE_ABI_ARRAY_BIT_NUMBERS)),
                                b.build_iconst_i32(0));
    Value* is_num_val = b.build_ule(val, imm(b, static_cast<int64_t>(kBronzeNumberMaxBits)));
    b.build_br_if(b.build_and(b.build_and(is_arr, is_bare), b.build_and(is_nums, is_num_val)),
                  bb_num, bb_fallback);

    b.position_at_end(bb_num);
    Value* f = b.build_bitcast_f64_i64(val);
    Value* u = b.build_fptosi_i32(f);
    Value* len = b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    Value* nonneg = b.build_sge(u, b.build_iconst_i32(0));
    Value* shrink = b.build_ule(u, len);
    Value* exact = b.build_eq(b.build_sitofp_f64_i32(u), f);
    b.build_br_if(b.build_and(b.build_and(nonneg, shrink), exact), bb_trunc, bb_fallback);

    b.position_at_end(bb_trunc);
    b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET, u);
    b.build_br_if(b.build_eq(u, b.build_iconst_i32(0)), bb_zero_head, bb_done);

    b.position_at_end(bb_zero_head);
    b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET, b.build_iconst_i32(0));
    b.build_br(bb_done);

    b.position_at_end(bb_done);
    b.build_ret_void();

    b.position_at_end(bb_fallback);
    b.build_call(prop_set_sym, Type::void_type(), {obj, key, val, site, mode});
    b.build_ret_void();
    fn->rebuild_cfg_predecessors();
}

}  // namespace il2mir
