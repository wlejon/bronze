// The NUMBERS bit of an Array (runtime/array.h) as generated code meets it:
// the claim `is.number_array` makes in front of a kernel's constant-index
// reads, and the one rule every inline element store owes it.
//
// THE RULE FOR A STORE. The bit promises that every element below `length`
// is a Number in double form. An inline store lands in bounds (or appends at
// `length`), so it can falsify the bit only by storing something that is not
// such a Number — and every such value is tagged above
// BRONZE_ABI_NUMBER_MAX_BITS, the same test that sends a reference to the
// write barrier. So a store clears the bit on that branch: free on a Number,
// one `and` on the header beside a barrier call otherwise. A store whose
// barrier the lowering proved away (a primitive that is not a reference, such
// as a `bool`) clears it by a select on the same test, unless the value is
// known to be a Number.
//
// Nothing generated SETS the bit except the claim's helper, which scans.

#include "il_abi.h"
#include "il_lowering_ops.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <cstdlib>
#include <string>

namespace il2mir {

namespace {

constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr uint64_t kTagMask = 0xFFFF000000000000ULL;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr uint64_t kPayloadLimit = uint64_t{1} << 48;

// A non-object's header and array words, for an in-process module: the zeroed
// block's header is no Array's, so branchless header/length/elements loads
// through it safely refuse non-objects.
alignas(16) const uint64_t kNotAnArray[8] = {};

Value* iconst(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

// The payload pointer of `obj` and whether it is an object at all.
void object_pointer(Builder& b, Value* obj, Value*& ptr, Value*& is_obj) {
    is_obj = b.build_eq(b.build_and(obj, iconst(b, kTagMask)), iconst(b, kObjectTagBits));
    ptr = b.build_and(obj, iconst(b, kPayloadMask));
}

}  // namespace

// Declared in il_lowering_ops.h. The f64 an i64 Value was boxed from here (box_f64_bits, IlLowering::ensure_type:
// a select of the canonical NaN and the bits), or null.
Value* boxed_f64_source(Value* v) {
    const Instruction* def = v ? v->defining_instruction() : nullptr;
    if (def && def->opcode() == Opcode::bitcast_i64_tagged && def->operand_count() >= 1) {
        const Instruction* inner = def->operand(0)->defining_instruction();
        if (inner && inner->opcode() == Opcode::bitcast_tagged_i64 && inner->operand_count() >= 1) {
            def = inner->operand(0)->defining_instruction();
        }
    }
    if (!def || def->opcode() != Opcode::select || def->operand_count() < 3) return nullptr;
    const Instruction* nan = def->operand(1)->defining_instruction();
    if (!nan || nan->opcode() != Opcode::iconst_i64 ||
        static_cast<uint64_t>(nan->imm_i64()) != BRONZE_ABI_CANONICAL_NAN_BITS) {
        return nullptr;
    }
    const Instruction* bits = def->operand(2)->defining_instruction();
    if (!bits || bits->opcode() != Opcode::bitcast_i64_f64 || bits->operand(0)->type() != Type::f64()) return nullptr;
    return bits->operand(0);
}

Value* PropertyLoweringHelper::lower_is_number_array(Builder& b, Value* obj, uint32_t max_index,
                                                     bool proven_dense) {
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "is_num_arr_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_obj = proven_dense ? nullptr : b.append_block(prefix + "_obj");
    BasicBlock* bb_arr = b.append_block(prefix + "_arr");
    // Named cold so block layout moves them out of the kernel: they run when
    // the bit is clear, which a numeric array leaves after its first claim.
    BasicBlock* bb_claim = b.append_block(prefix + "_claim_cold");
    BasicBlock* bb_call = b.append_block(prefix + "_call_cold");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i32());

    b.position_at_end(cur);
    Value* ptr = nullptr;
    Value* in_bounds = nullptr;
    Value* len = nullptr;
    if (proven_dense) {
        // An Array with `max_index` below its length, proven on this path: only
        // the bit is left to read.
        ptr = b.build_and(obj, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
        b.build_br(bb_arr);
        b.position_at_end(bb_arr);
        len = b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    } else {
        Value* is_obj = nullptr;
        object_payload(b, obj, ptr, is_obj);
        b.build_br_if(is_obj, bb_obj, {}, bb_merge, {b.build_iconst_i32(0)});

        b.position_at_end(bb_obj);
        Value* is_arr = b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kArrayHeaderLow));
        b.build_br_if(is_arr, bb_arr, {}, bb_merge, {b.build_iconst_i32(0)});

        b.position_at_end(bb_arr);
        len = b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
        in_bounds = b.build_ult(b.build_iconst_i32(static_cast<int32_t>(max_index)), len);
    }
    Value* bits = b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET);
    Value* has_bit = b.build_ne(b.build_and(bits, b.build_iconst_i32(BRONZE_ABI_ARRAY_BIT_NUMBERS)),
                                b.build_iconst_i32(0));
    b.build_br_if(proven_dense ? has_bit : b.build_and(in_bounds, has_bit), bb_merge, {b.build_iconst_i32(1)},
                  bb_claim, {});

    // The bit is clear: an array short enough to scan gets one, and its answer
    // is the claim's. One too long, or too short for the reads, is answered
    // no without a call.
    b.position_at_end(bb_claim);
    Value* scannable = b.build_ule(len, b.build_iconst_i32(BRONZE_ABI_ARRAY_CLAIM_MAX_LENGTH));
    b.build_br_if(proven_dense ? scannable : b.build_and(in_bounds, scannable), bb_call, {}, bb_merge,
                  {b.build_iconst_i32(0)});

    b.position_at_end(bb_call);
    Value* claimed = b.build_call("bronze_array_claim_numbers", Type::i32(), {obj});
    b.build_br(bb_merge, {claimed});

    b.position_at_end(bb_merge);
    return result;
}

void PropertyLoweringHelper::forget_numbers(Builder& b, Value* obj) {
    Value* ptr = b.build_and(obj, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
    Value* bits = b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET);
    b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET,
                  b.build_and(bits, b.build_iconst_i32(~static_cast<int32_t>(BRONZE_ABI_ARRAY_BIT_NUMBERS))));
}

void PropertyLoweringHelper::forget_numbers_unless_number(Builder& b, Value* obj, Value* val) {
    Value* ptr = b.build_and(obj, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
    Value* not_num = b.build_ugt(val, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
    Value* mask = b.build_select(not_num, b.build_iconst_i32(~static_cast<int32_t>(BRONZE_ABI_ARRAY_BIT_NUMBERS)),
                                 b.build_iconst_i32(-1));
    Value* bits = b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET);
    b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET, b.build_and(bits, mask));
}

// THE HOLE RULE. A hole is no own element, so its read continues up the chain
// and a store into it (or at `length`) is a Set the chain may take: a setter,
// a non-writable element, a proxy (10.1.8.1, 10.1.9.1). The runtime asks the
// chain; generated code asks only the hazard, which says whether any chain
// could answer, and only on a hole or an append — an element that is there is
// read and written exactly as before. A dense-array guard asks it for the
// reads and writes it licenses (emit_dense_array_guard), since those treat a
// hole as `undefined` or store over one without looking — through the tag
// mask it ANDs the elements word with, which the TLS block holds and a set
// hazard zeroes, so the question costs the guard nothing (a separate load and
// test of the hazard there was +2.8% instructions on three_math).
namespace {

Value* tls_block_word(Builder& b, int32_t off) {
    Module* mod = b.current_block()->parent()->parent();
    Value* tls = (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                                     : b.build_call("bronze_tls_block_addr", Type::i64(), {});
    return b.build_load(Type::i64(), tls, off);
}

}  // namespace

Value* PropertyLoweringHelper::index_hazard_tag_mask(Builder& b) {
    return tls_block_word(b, BRONZE_TLS_ARRAY_ELEMS_TAG_MASK_OFF);
}

Value* PropertyLoweringHelper::index_hazard_word(Builder& b) {
    return tls_block_word(b, BRONZE_TLS_ARRAY_INDEX_HAZARD_OFF);
}

Value* PropertyLoweringHelper::index_hazard_clear(Builder& b) {
    return b.build_eq(tls_block_word(b, BRONZE_TLS_ARRAY_INDEX_HAZARD_OFF), b.build_iconst_i64(0));
}

// append_block moves the builder onto the new block; these callers are still
// emitting the block that branches to it.
BasicBlock* PropertyLoweringHelper::append_block_keeping_position(Builder& b, const std::string& name) {
    BasicBlock* here = b.current_block();
    BasicBlock* added = b.append_block(name);
    b.position_at_end(here);
    return added;
}

void PropertyLoweringHelper::finish_hole_read(Builder& b, Value* elem, BasicBlock* done, BasicBlock* slow,
                                              const std::string& prefix) {
    BasicBlock* bb_hole = append_block_keeping_position(b, prefix + "_hole_cold");
    Value* is_hole = b.build_eq(elem, b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_HOLE_BITS)));
    b.build_br_if(is_hole, bb_hole, {}, done, {elem});
    b.position_at_end(bb_hole);
    b.build_br_if(index_hazard_clear(b), done, {b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag))}, slow, {});
}

void PropertyLoweringHelper::branch_on_hole_store(Builder& b, Value* block, Value* slot, BasicBlock* store,
                                                  BasicBlock* slow, const std::string& prefix) {
    BasicBlock* bb_hole = append_block_keeping_position(b, prefix + "_hole_cold");
    Value* old = b.build_load_indexed(Type::i64(), block, slot, 8, static_cast<int32_t>(kBronzeHdrBytes));
    Value* is_hole = b.build_eq(old, b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_HOLE_BITS)));
    b.build_br_if(is_hole, bb_hole, store);
    b.position_at_end(bb_hole);
    b.build_br_if(index_hazard_clear(b), store, slow);
}

// The two stores an Array takes inline, both what the helper would do
// (rt_prop_array.cpp rtArrayIndexSet):
//  - an index below `length`: its own element, or a hole while the hazard is
//    clear (branch_on_hole_store);
//  - the APPEND, an index equal to `length` inside the block, while the hazard
//    is clear. The array owns no such element, so 10.1.9.2 asks the chain,
//    which with the hazard clear holds no index anywhere; the write is then
//    CreateDataProperty on an extensible array (no properties object means no
//    integrity level, runtime/integrity.h) and moves `length` by one — what
//    the inline push does (il_array_method.cpp), for `a[a.length] = v` and the
//    `a[n++] = v` fill loops that spell a push that way. The slot past
//    `length` is a hole (the block is hole-filled there), so nothing is
//    overwritten, and `length` is stored before the element with no call or
//    allocation between them.
// Both need no properties object: frozen, sealed and accessor elements live
// behind it.
void PropertyLoweringHelper::branch_on_array_store(Builder& b, Value* ptr, Value* idx, BasicBlock* store,
                                                   BasicBlock* slow, const std::string& prefix, Value*& out_block,
                                                   Value*& out_slot) {
    Value* length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
    Value* capacity = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET));
    Value* head = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET));
    Value* slot = b.build_add(idx, head);
    Value* props = b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
    Value* no_props = b.build_ne(b.build_and(props, iconst(b, kTagMask)), iconst(b, kObjectTagBits));
    Value* in_bounds = b.build_and(b.build_ult(idx, length), b.build_ult(slot, capacity));
    // The block word is loaded here, where it dominates both stores; with no
    // properties object an Array always has one once its capacity is nonzero,
    // and a capacity of zero passes neither bound below.
    Value* block = b.build_and(b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET), iconst(b, kPayloadMask));
    BasicBlock* bb_slot = append_block_keeping_position(b, prefix + "_array_slot");
    BasicBlock* bb_not_own = append_block_keeping_position(b, prefix + "_array_notown");
    BasicBlock* bb_append = append_block_keeping_position(b, prefix + "_array_append");
    b.build_br_if(b.build_and(in_bounds, no_props), bb_slot, bb_not_own);

    b.position_at_end(bb_slot);
    branch_on_hole_store(b, block, slot, store, slow, prefix);

    // The hazard word folds into the capacity bound, as the push's does: a
    // slot OR'd with it is below no capacity once it is set.
    b.position_at_end(bb_not_own);
    Value* at_end = b.build_eq(idx, length);
    Value* fits = b.build_ult(b.build_or(slot, index_hazard_word(b)), capacity);
    b.build_br_if(b.build_and(b.build_and(at_end, fits), no_props), bb_append, slow);

    b.position_at_end(bb_append);
    b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET, b.build_trunc_i32(b.build_add(idx, iconst(b, 1))));
    b.build_br(store);
    out_block = block;
    out_slot = slot;
}

bool PropertyLoweringHelper::is_known_number(Value* val) const {
    if (!val) return false;
    if (val->type() == Type::f64()) return true;
    if (boxed_f64_source(val) != nullptr) return true;
    const Instruction* def = val->defining_instruction();
    if (def && def->opcode() == Opcode::iconst_i64) {
        return static_cast<uint64_t>(def->imm_i64()) <= kBronzeNumberMaxBits;
    }
    return false;
}

void PropertyLoweringHelper::emit_dense_array_guard(Builder& b, Value* obj, uint32_t max_index, bool writable,
                                                    Value*& out_ok, Value*& out_block, Value*& out_head,
                                                    BasicBlock* miss) {
    const bool can_select = in_process_ || (b.current_module() && b.current_module()->pinned_tls_register());
    // `header_known`: the header test was a branch already taken.
    auto emit_header_checks = [&](Value* base, bool header_known, Value*& ok, Value*& block, Value*& head) {
        Value* is_array = header_known
            ? nullptr
            : b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kArrayHeaderLow));
        auto with_array = [&](Value* v) { return is_array ? b.build_and(is_array, v) : v; };
        Value* elems = b.build_load(Type::i64(), base, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
        // A run reads a hole as `undefined` and stores over one, so the hole
        // rule's TLS tag mask (il_number_array.cpp) fails it once tripped.
        Value* has_elems = b.build_eq(b.build_and(elems, index_hazard_tag_mask(b)), iconst(b, kObjectTagBits));
        Value* raw_block = b.build_and(elems, iconst(b, kPayloadMask));
        Value* raw_head = b.build_zext_i64(b.build_load(Type::i32(), base, BRONZE_ABI_ARRAY_HEAD_OFFSET));
        block = b.build_add(raw_block, b.build_shl(raw_head, iconst(b, 3)));
        head = nullptr;
        if (!writable) {
            Value* len = b.build_load(Type::i32(), base, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
            Value* in_bounds = b.build_ult(b.build_iconst_i32(static_cast<int32_t>(max_index)), len);
            // has_elems first: brass's register allocation loses the vreg of
            // a `br_if` on `and(ult.i32 const, load.i32, ...)` (in_bounds
            // first) once nothing else is ANDed in front of it.
            ok = with_array(b.build_and(has_elems, in_bounds));
        } else {
            Value* len = b.build_zext_i64(b.build_load(Type::i32(), base, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
            Value* cap = b.build_zext_i64(b.build_load(Type::i32(), base, BRONZE_ABI_ARRAY_CAPACITY_OFFSET));
            Value* max_slot = max_index == 0 ? raw_head : b.build_add(raw_head, iconst(b, max_index));
            Value* in_bounds = b.build_and(b.build_ult(iconst(b, max_index), len), b.build_ult(max_slot, cap));
            Value* props = b.build_load(Type::i64(), base, BRONZE_ABI_ARRAY_PROPS_OFFSET);
            Value* no_props = b.build_ne(b.build_and(props, iconst(b, kTagMask)), iconst(b, kObjectTagBits));
            ok = b.build_and(with_array(no_props), b.build_and(in_bounds, has_elems));
        }
    };

    if (miss) {
        // The object and header tests as predicted branches to the caller's
        // miss: the array's words are read through its payload with no
        // select in the address, and `out_ok` is what is left.
        BasicBlock* cur = b.current_block();
        const std::string prefix = "dense_guard_" + std::to_string(cur->parent()->next_block_id());
        BasicBlock* bb_obj = b.append_block(prefix + "_obj");
        BasicBlock* bb_arr = b.append_block(prefix + "_arr");
        b.position_at_end(cur);
        Value* ptr = b.build_xor(obj, iconst(b, kObjectTagBits));
        b.build_br_if(b.build_ult(ptr, iconst(b, kPayloadLimit)), bb_obj, miss);
        b.position_at_end(bb_obj);
        b.build_br_if(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kArrayHeaderLow)), bb_arr,
                      miss);
        b.position_at_end(bb_arr);
        emit_header_checks(ptr, /*header_known=*/true, out_ok, out_block, out_head);
        return;
    }

    if (can_select) {
        Value* ptr = b.build_xor(obj, iconst(b, kObjectTagBits));
        Value* is_obj = b.build_ult(ptr, iconst(b, kPayloadLimit));
        Value* not_arr = in_process_
            ? iconst(b, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&kNotAnArray[0])))
            : b.build_pinned_tls_read();
        Value* base = b.build_select(is_obj, ptr, not_arr);
        // Either stand-in's first word is no header (the zeroed block, or the
        // TLS block's even proto epoch: bronze_abi.h), so the header compare
        // alone refuses a non-object.
        emit_header_checks(base, /*header_known=*/false, out_ok, out_block, out_head);
        return;
    }

    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "dense_guard_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_arr = b.append_block(prefix + "_arr");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    out_ok = b.add_block_param(bb_merge, Type::i32());
    out_block = b.add_block_param(bb_merge, Type::i64());
    out_head = nullptr;

    b.position_at_end(cur);
    Value* ptr = nullptr;
    Value* is_obj = nullptr;
    object_pointer(b, obj, ptr, is_obj);
    b.build_br_if(is_obj, bb_arr, {}, bb_merge, {b.build_iconst_i32(0), iconst(b, 0)});

    b.position_at_end(bb_arr);
    Value* ok = nullptr;
    Value* block = nullptr;
    Value* head = nullptr;
    emit_header_checks(ptr, /*header_known=*/false, ok, block, head);
    b.build_br(bb_merge, {ok, block});

    b.position_at_end(bb_merge);
}

}  // namespace il2mir
