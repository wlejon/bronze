// The write barrier's inline filter (il_write_barrier.h).
//
// brass's barrier (gc/heap.hpp, write_barrier_interior) does nothing for a
// target outside the old generation, and the young generation never overlaps
// it, so a store into a young object never needs the call. Those are most of
// the stores generated code barriers: a fresh object's fields, an iteration
// record's `current` on every step of a for-of over objects, the arrays a
// frame builds. On a three.js-shaped scene every barrier call counted had a
// young target (bench/mesh_churn_2k.js: 73.6K of 78.4K, the rest not
// references at all); on a 32-element for-of over objects all 32M were.
//
// The range is the heap's young reservation, fixed for its lifetime, so the
// test is sound at any point, a marking cycle included: it never skips a
// store the slow path would act on. The value-range half of brass's filter
// (barrier_value_base / span) is NOT inlined: it moves when a marking cycle
// opens, and a copy that lagged would skip a shade.

#include "il_write_barrier.h"

#include "abi/bronze_abi_tls.h"
#include "il_abi.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <string>

namespace il2mir {

using brass::BasicBlock;
using brass::Module;
using brass::Type;

void build_write_barrier(Builder& b, Value* obj, Value* val) {
    BasicBlock* cur = b.current_block();
    Module* mod = cur->parent()->parent();
    if (!mod || !mod->pinned_tls_register()) {
        b.build_call("brass_gc_write_barrier", Type::void_type(), {obj, val});
        return;
    }
    const std::string prefix = "wb_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_call = b.append_block(prefix + "_call_cold");
    BasicBlock* bb_done = b.append_block(prefix + "_done");

    b.position_at_end(cur);
    Value* tls = b.build_pinned_tls_read();
    Value* address = b.build_and(obj, b.build_iconst_i64(static_cast<int64_t>(0x0000FFFFFFFFFFFFULL)));
    Value* young_base = b.build_load(Type::i64(), tls, BRONZE_TLS_YOUNG_BASE_OFF);
    Value* young_span = b.build_load(Type::i64(), tls, BRONZE_TLS_YOUNG_SPAN_OFF);
    Value* young = b.build_ult(b.build_sub(address, young_base), young_span);
    b.build_br_if(young, bb_done, bb_call);

    b.position_at_end(bb_call);
    b.build_call("brass_gc_write_barrier", Type::void_type(), {obj, val});
    b.build_br(bb_done);

    b.position_at_end(bb_done);
}

}  // namespace il2mir
