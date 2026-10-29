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
//
// BRONZE_NO_INLINE_BOX_STR_KEY=1 keeps every literal on the helper.

#include "il_lowering_ops.h"
#include "il_abi.h"
#include <cstdlib>

namespace il2mir {

Value* lower_box_str_key(Builder& b, Value* key_id) {
    static const bool disabled = std::getenv("BRONZE_NO_INLINE_BOX_STR_KEY") != nullptr;
    BasicBlock* cur = b.current_block();
    Module* mod = cur->parent()->parent();
    // Without the pinned TLS register the block's address is itself a call;
    // the helper is then no dearer.
    if (disabled || !mod || !mod->pinned_tls_register()) {
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

} // namespace il2mir
