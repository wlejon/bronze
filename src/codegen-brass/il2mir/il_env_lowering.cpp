// Environment record reads and writes, inline.
//
// A captured binding's depth and index are constants of the scope plan, and a
// record's layout is fixed where it is created (runtime/env.h: the parent link
// at BRONZE_ABI_ENV_PARENT_OFFSET, the slots from BRONZE_ABI_ENV_SLOTS_OFFSET),
// so the read is `depth` parent loads and one slot load. The helpers these
// replace check the record's kind and the slot's range and fatal on a miss:
// both questions are the lowering's to get right, and nothing a program does
// can change their answer, so the inline form asks neither. A closure's record
// reaches it through its function object, which create.func set once.
//
// Through a helper each read was a call (667k in instanced_mesh_churn's
// compute region, half of three.js's module-scope `const` reads and every
// closure's captured variable).

#include "il_lowering.h"

namespace il2mir {

namespace {

Value* iconst64(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

constexpr uint64_t kEnvObjectTagBits = static_cast<uint64_t>(BRONZE_ABI_TAG_OBJECT) << BRONZE_ABI_VALUE_TAG_SHIFT;

// The record `depth` parent links above `env` (a tagged record), as a raw
// pointer to its header.
Value* env_record_ptr(Builder& b, Value* env, uint32_t depth) {
    Value* ptr = b.build_xor(env, iconst64(b, kEnvObjectTagBits));
    for (uint32_t d = 0; d < depth; ++d) {
        Value* parent = b.build_load(Type::i64(), ptr, BRONZE_ABI_ENV_PARENT_OFFSET);
        ptr = b.build_xor(parent, iconst64(b, kEnvObjectTagBits));
    }
    return ptr;
}

int32_t slot_offset(uint32_t index) {
    return static_cast<int32_t>(BRONZE_ABI_ENV_SLOTS_OFFSET + static_cast<uint64_t>(index) * 8u);
}

}  // namespace

Value* IlLowering::lower_env_get(Builder& b, Value* env, uint32_t depth, uint32_t index) {
    Value* rec = env_record_ptr(b, ensure_type(env, Type::i64(), b), depth);
    return b.build_load(Type::i64(), rec, slot_offset(index));
}

// The TDZ read: the uninitialized marker can never be a program's value, so one
// compare against it is the whole check (ECMA-262 9.1.1.1.6). The helper is
// kept for the miss, where it names the binding in the ReferenceError it throws.
Value* IlLowering::lower_env_get_tdz(Builder& b, Value* env, uint32_t depth, uint32_t index, Value* key) {
    Value* env_bits = ensure_type(env, Type::i64(), b);
    Value* rec = env_record_ptr(b, env_bits, depth);
    Value* bits = b.build_load(Type::i64(), rec, slot_offset(index));

    BasicBlock* cur = b.current_block();
    const std::string prefix = "env_tdz_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_uninit = b.append_block(prefix + "_uninit");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i64());

    b.position_at_end(cur);
    Value* uninit = b.build_eq(bits, iconst64(b, BRONZE_ABI_UNINITIALIZED_BITS));
    b.build_br_if(uninit, bb_uninit, {}, bb_merge, {bits});

    b.position_at_end(bb_uninit);
    Value* thrown = b.build_call("bronze_env_get_tdz", Type::i64(),
                                 {env_bits, b.build_iconst_i32(static_cast<int32_t>(depth)),
                                  b.build_iconst_i32(static_cast<int32_t>(index)), key});
    b.build_br(bb_merge, {thrown});

    b.position_at_end(bb_merge);
    return result;
}

// The write, with the collector's barrier for a reference: a record outlives
// most of what is stored in it (a module scope's, a constructor's captured
// callbacks), so an old record pointing at a young value is the common case.
void IlLowering::lower_env_set(Builder& b, Value* env, uint32_t depth, uint32_t index, Value* val,
                               bool val_is_primitive) {
    Value* rec = env_record_ptr(b, ensure_type(env, Type::i64(), b), depth);
    Value* bits = ensure_type(val, Type::i64(), b);
    b.build_store(Type::i64(), rec, slot_offset(index), bits);
    if (val_is_primitive || prop_lowering_.is_known_non_ref(bits)) return;

    BasicBlock* cur = b.current_block();
    const std::string prefix = "env_set_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_barrier = b.append_block(prefix + "_barrier");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    b.position_at_end(cur);
    Value* is_ref = b.build_ugt(bits, iconst64(b, BRONZE_ABI_NUMBER_MAX_BITS));
    b.build_br_if(is_ref, bb_barrier, bb_merge);
    b.position_at_end(bb_barrier);
    Value* slot_addr = b.build_add(rec, iconst64(b, static_cast<uint64_t>(slot_offset(index))));
    b.build_call("brass_gc_write_barrier", Type::void_type(), {slot_addr, bits});
    b.build_br(bb_merge);
    b.position_at_end(bb_merge);
}

}  // namespace il2mir
