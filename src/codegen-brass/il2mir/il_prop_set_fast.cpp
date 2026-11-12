// kPropSetStub (il_property.h): the write stub every cached write site calls.
//
// build_prop_set_stub (il_prop_stubs.cpp, kPropSetSlowStub) answers a site's
// write from its ways, the thread's megamorphic table, a setter on the chain
// and the transition way 0 names. It stages a setter's argument in a traced
// slot and may call, so every call of it saved four registers, spilled its
// five operands and loaded all of the site's ways before it tested one: 110
// instructions for an add (a listener object's fields in a plain function
// constructor, a field a setter adds), which is most of what reaches it.
//
// This is its two answers that call nothing but the barrier, from way 0
// alone: the own data slot, and the add into an inline slot. Each test is a compare and a branch of its
// own against an immediate or a memory operand, as il_prop_get_fast.cpp's
// are and for its reason, so the paths that answer use argument and scratch
// registers only. Everything else goes to the full stub with the same
// operands, which answers the whole write again.
#include "il_prop_set_fast.h"

#include "il_abi.h"
#include "il_lowering.h"
#include "il_property.h"

#include <string>

namespace il2mir {

namespace {

constexpr int32_t kPlainHeader = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;

Value* imm(Builder& b, int64_t v) { return b.build_iconst_i64(v); }

// The low 48 bits of a word (a payload, or the overflow block's address).
Value* payload(Builder& b, Value* word) { return b.build_lshr(b.build_shl(word, imm(b, 16)), imm(b, 16)); }

// Whether `word` is a Value with the object tag.
Value* object_tagged(Builder& b, Value* word) {
    return b.build_eq(b.build_lshr(word, imm(b, 48)), imm(b, BRONZE_ABI_TAG_OBJECT));
}

// A chain of blocks each ending in one test: `ok` goes on, a failure leaves
// for `miss`.
struct Tests {
    Builder& b;
    BasicBlock* miss;
    std::string prefix;
    int n = 0;
    void require(Value* ok) {
        BasicBlock* cur = b.current_block();
        BasicBlock* next = b.append_block(prefix + std::to_string(n++));
        b.position_at_end(cur);
        b.build_br_if(ok, next, miss);
        b.position_at_end(next);
    }
};

}  // namespace

void build_prop_set_fast_stub(Function* fn) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    Value* obj = b.add_block_param(entry, Type::i64());
    Value* key_id = b.add_block_param(entry, Type::i32());
    Value* val = b.add_block_param(entry, Type::i64());
    Value* site = b.add_block_param(entry, Type::ptr());
    Value* strict = b.add_block_param(entry, Type::i32());
    const std::string p = "psfast";
    BasicBlock* slow = b.append_block(p + "_slow");
    Value* slow_obj = b.add_block_param(slow, Type::i64());
    BasicBlock* miss = b.append_block(p + "_miss");
    BasicBlock* object = b.append_block(p + "_object");
    BasicBlock* way0 = b.append_block(p + "_way0");
    BasicBlock* add = b.append_block(p + "_add");
    BasicBlock* own = b.append_block(p + "_own");
    BasicBlock* store = b.append_block(p + "_store");
    BasicBlock* barrier = b.append_block(p + "_barrier");
    Value* slot_addr = b.add_block_param(barrier, Type::i64());
    BasicBlock* done = b.append_block(p + "_done");

    // A plain object: the object tag, then the header's flags.
    b.position_at_end(entry);
    b.build_br_if(object_tagged(b, obj), object, {}, slow, {obj});
    b.position_at_end(object);
    Value* ptr = payload(b, obj);
    Value* shape = b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kPlainHeader)), way0, {}, slow, {obj});
    b.position_at_end(way0);
    b.build_br_if(b.build_eq(shape, b.build_load(Type::i64(), site, BRONZE_ABI_IC_SHAPE_OFFSET)), own, add);

    // The add: PropertyLoweringHelper::emit_transition_arm's tests, which
    // says what each is for. An unfilled entry (zero) and the array-method
    // sentinel are no shape to read a parent from. The tests that read the
    // receiver's shape come first and the slot last, so no more than three
    // values are held at once.
    b.position_at_end(add);
    {
        Tests t{b, miss, p + "_t"};
        Value* cached = b.build_load(Type::i64(), site, BRONZE_ABI_IC_SHAPE_OFFSET);
        t.require(b.build_ugt(cached, imm(b, BRONZE_ABI_IC_SHAPE_ARRAY_METHOD)));
        t.require(b.build_eq(shape, b.build_load(Type::i64(), cached, BRONZE_ABI_SHAPE_PARENT_OFFSET)));
        t.require(b.build_ult(b.build_load(Type::i64(), shape, BRONZE_ABI_SHAPE_DICT_OFFSET), imm(b, 1)));
        t.require(b.build_ult(b.build_zext_i64(b.build_load(Type::i8(), shape, BRONZE_ABI_SHAPE_USEDPROTO_OFFSET)),
                              imm(b, 1)));
        t.require(b.build_eq(b.build_load(Type::i64(), shape, BRONZE_ABI_SHAPE_BARE_HEADER_OFFSET),
                             b.build_load(Type::i64(), ptr, 0)));
        t.require(b.build_eq(b.build_load(Type::i32(), cached, BRONZE_ABI_SHAPE_ATTRS_OFFSET),
                             b.build_iconst_i32(static_cast<int32_t>(BRONZE_ABI_SHAPE_ATTRS_PLAIN_DATA))));
        t.require(b.build_eq(b.build_load(Type::i64(), b.build_pinned_tls_read(), BRONZE_TLS_PROTO_EPOCH_OFF),
                             b.build_load(Type::i64(), site, BRONZE_ABI_IC_EPOCH_OFFSET)));
        // The shape's own slot is the entry's slot word, so the word has no
        // flag; and it is an inline slot (one in the overflow block is the
        // full stub's, which asks the block for room).
        Value* slot = b.build_zext_i64(b.build_load(Type::i32(), cached, BRONZE_ABI_SHAPE_SLOTINDEX_OFFSET));
        t.require(b.build_eq(slot, b.build_load(Type::i64(), site, BRONZE_ABI_IC_SLOT_OFFSET)));
        t.require(b.build_ult(slot, imm(b, kBronzeObjOverflowSlot)));
        b.build_store(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET, cached);
        Value* at = b.build_add(b.build_add(ptr, b.build_shl(slot, imm(b, 3))), imm(b, kBronzeObjSlotsOffset));
        b.build_store(Type::i64(), at, 0, val);
        b.build_br_if(b.build_ugt(val, imm(b, static_cast<int64_t>(kBronzeNumberMaxBits))), barrier, {at}, done, {});
    }

    // An own data slot of way 0's shape: the slot word has no flag.
    b.position_at_end(own);
    Value* word = b.build_load(Type::i64(), site, BRONZE_ABI_IC_SLOT_OFFSET);
    b.build_br_if(b.build_eq(b.build_lshr(word, imm(b, 32)), imm(b, 0)), store, miss);

    b.position_at_end(store);
    {
        Value* is_inline = b.build_ult(word, imm(b, kBronzeObjOverflowSlot));
        Value* overflow = payload(b, b.build_load(Type::i64(), ptr, kBronzeObjOverflowOffset));
        constexpr int64_t kShift = kBronzeObjSlotsOffset + kBronzeObjOverflowSlot * 8 - kBronzeHdrBytes;
        Value* base = b.build_select(is_inline, ptr, b.build_sub(overflow, imm(b, kShift)));
        Value* at = b.build_add(b.build_add(base, b.build_shl(word, imm(b, 3))), imm(b, kBronzeObjSlotsOffset));
        b.build_store(Type::i64(), at, 0, val);
        b.build_br_if(b.build_ugt(val, imm(b, static_cast<int64_t>(kBronzeNumberMaxBits))), barrier, {at}, done, {});
    }

    b.position_at_end(barrier);
    b.build_write_barrier(slot_addr, val);
    b.build_br(done);

    b.position_at_end(done);
    b.build_ret_void();

    // A miss before the receiver is known to be a plain object passes it as
    // it came; every later one boxes it again from its payload (the tag was
    // tested), so the argument's register is free past the header check.
    b.position_at_end(miss);
    b.build_br(slow, {b.build_or(ptr, imm(b, static_cast<int64_t>(uint64_t{BRONZE_ABI_TAG_OBJECT} << 48)))});
    b.position_at_end(slow);
    b.build_call(kPropSetSlowStub, Type::void_type(), {slow_obj, key_id, val, site, strict});
    b.build_ret_void();
    fn->rebuild_cfg_predecessors();
}

}  // namespace il2mir
