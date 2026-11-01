// `box.str k`: a string literal as a Value, which is bronze_box_str_key's
// answer — the thread's arena header for global key k, tagged as a String.
//
// The helper is a call plus a bounds check and a load from a thread_local
// vector (rt_key.cpp, rtKeyHeader), ~43 instructions; three.js pays it per
// Euler set for `case 'XYZ':` in Quaternion.setFromEuler. The runtime mirrors
// that vector into the TLS block (bronze_abi_tls.h, key_header_tbl /
// key_header_count) after every change of its storage, so the inline path
// reads the same entry the helper would: present and nonzero, it is the
// answer; past the count or 0 (not interned on this thread yet), the helper
// runs and interns it, and the next evaluation hits.

#include "il_lowering_ops.h"
#include "il_abi.h"

namespace il2mir {

Value* lower_box_str_key(Builder& b, Value* key_id) {
    BasicBlock* cur = b.current_block();
    Module* mod = cur->parent()->parent();
    // Without the pinned TLS register the block's address is itself a call;
    // the helper is then no dearer.
    if (!mod || !mod->pinned_tls_register()) {
        return b.build_call("bronze_box_str_key", Type::i64(), {key_id});
    }
    const std::string prefix = "boxkey_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* probe = b.append_block(prefix + "_probe");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    Value* tls = b.build_pinned_tls_read();
    Value* idx = b.build_zext_i64(key_id);
    Value* count = b.build_load(Type::i64(), tls, BRONZE_TLS_KEY_HEADER_COUNT_OFF);
    b.build_br_if(b.build_ult(idx, count), probe, slow);

    // Only below the count is the entry inside the table.
    b.position_at_end(probe);
    Value* tbl = b.build_load(Type::i64(), tls, BRONZE_TLS_KEY_HEADER_TBL_OFF);
    Value* header = b.build_load(Type::i64(), b.build_add(tbl, b.build_shl(idx, b.build_iconst_i64(3))), 0);
    Value* tagged = b.build_or(header, b.build_iconst_i64(static_cast<int64_t>(uint64_t{BRONZE_ABI_TAG_STRING} << 48)));
    b.build_br_if(b.build_ne(header, b.build_iconst_i64(0)), merge, {tagged}, slow, {});

    b.position_at_end(slow);
    slow->set_cold();
    b.build_br(merge, {b.build_call("bronze_box_str_key", Type::i64(), {key_id})});

    b.position_at_end(merge);
    return result;
}

// `x === 'XYZ'`, the literal a `box.str`: setFromEuler's `switch (order)`.
// The literal is a String, so `===` is true exactly when `x` is a String with
// its text, and the Number test the general lowering opens with
// (lower_dynamic_strict_eq) answers nothing here: a Number is never ===
// a String. The literal's bits are the key header the shapes and every other
// evaluation of the same literal on this thread hold, so the common true
// answer — the value IS that literal, stored by a default parameter or a
// field write — is one compare. A value with other bits is false unless it is
// a String, and then the shared length / memoized-hash test, then the helper's
// contents compare, decide as they do for any two Strings.
Value* lower_strict_eq_string_literal(Builder& b, Value* x, Value* lit) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "seqlit_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* differ = b.append_block(prefix + "_differ");
    BasicBlock* strings = b.append_block(prefix + "_strings");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i32());

    b.position_at_end(cur);
    b.build_br_if(b.build_eq(x, lit), merge, {b.build_iconst_i32(1)}, differ, {});

    b.position_at_end(differ);
    Value* tag = b.build_lshr(x, b.build_iconst_i64(BRONZE_ABI_VALUE_TAG_SHIFT));
    b.build_br_if(b.build_eq(tag, b.build_iconst_i64(BRONZE_ABI_TAG_STRING)), strings, {}, merge,
                  {b.build_iconst_i32(0)});

    b.position_at_end(strings);
    Value* mask = b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    Value* proven = build_strings_differ(b, b.build_and(x, mask), b.build_and(lit, mask));
    b.build_br_if(proven, merge, {b.build_iconst_i32(0)}, slow, {});

    b.position_at_end(slow);
    slow->set_cold();
    b.build_br(merge, {b.build_and(b.build_call("bronze_strict_eq", Type::i32(), {x, lit}),
                                   b.build_iconst_i32(1))});

    b.position_at_end(merge);
    return result;
}

} // namespace il2mir
