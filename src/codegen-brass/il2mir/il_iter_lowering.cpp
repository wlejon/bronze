// The iteration record's value and close, inline (runtime/iterator.h, laid
// out in bronze_abi_tls.h "the iteration record").

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
