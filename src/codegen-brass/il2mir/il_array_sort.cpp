// The module's merge stub for `Array.prototype.sort` (il_array_sort.h says
// why it exists and what it promises). The loop below is mergeRuns and the
// width loop around it in runtime/builtin_array_sort.cpp, block for block.
#include "il_array_sort.h"

#include "il_abi.h"
#include "il_lowering_direct.h"

#include <brass/mir/function.hpp>

namespace il2mir {

namespace {

Value* iconst(Builder& b, int64_t v) { return b.build_iconst_i64(v); }

// The smaller of `a` and `b`, both counts below 2^33.
Value* umin(Builder& b, Value* a, Value* b2) { return b.build_select(b.build_ult(a, b2), a, b2); }

}  // namespace

void build_sort_merge_stub(Function* fn) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    (void)b.add_block_param(entry, Type::i64());  // env
    (void)b.add_block_param(entry, Type::i64());  // this
    (void)b.add_block_param(entry, Type::i32());  // argc
    Value* frame = b.add_block_param(entry, Type::ptr());

    // The loop state is addresses into the two halves, not indices, so that
    // what lives across the comparator call is five words.
    BasicBlock* width_head = b.append_block("sort_width");
    Value* width = b.add_block_param(width_head, Type::i64());
    Value* src = b.add_block_param(width_head, Type::i64());
    Value* dst = b.add_block_param(width_head, Type::i64());
    BasicBlock* run_head = b.append_block("sort_run");
    Value* lo = b.add_block_param(run_head, Type::i64());
    BasicBlock* run_body = b.append_block("sort_run_body");
    BasicBlock* merge_head = b.append_block("sort_merge");
    Value* pi = b.add_block_param(merge_head, Type::i64());
    Value* pj = b.add_block_param(merge_head, Type::i64());
    Value* pk = b.add_block_param(merge_head, Type::i64());
    BasicBlock* merge_body = b.append_block("sort_merge_body");
    BasicBlock* undef = b.append_block("sort_undef");
    BasicBlock* call = b.append_block("sort_call");
    BasicBlock* call_direct = b.append_block("sort_call_direct");
    BasicBlock* call_dynamic = b.append_block("sort_call_dynamic");
    BasicBlock* answered = b.append_block("sort_answered");
    Value* answer = b.add_block_param(answered, Type::i64());
    BasicBlock* to_number = b.append_block("sort_to_number");
    BasicBlock* pick = b.append_block("sort_pick");
    Value* take_left = b.add_block_param(pick, Type::i64());
    BasicBlock* take_l = b.append_block("sort_take_left");
    BasicBlock* take_r = b.append_block("sort_take_right");
    BasicBlock* left_head = b.append_block("sort_left");
    Value* li = b.add_block_param(left_head, Type::i64());
    Value* lj = b.add_block_param(left_head, Type::i64());
    Value* lk = b.add_block_param(left_head, Type::i64());
    BasicBlock* left_copy = b.append_block("sort_left_copy");
    BasicBlock* right_head = b.append_block("sort_right");
    Value* rj = b.add_block_param(right_head, Type::i64());
    Value* rk = b.add_block_param(right_head, Type::i64());
    BasicBlock* right_copy = b.append_block("sort_right_copy");
    BasicBlock* run_next = b.append_block("sort_run_next");
    BasicBlock* width_next = b.append_block("sort_width_next");
    BasicBlock* done = b.append_block("sort_done");

    b.position_at_end(entry);
    Value* count = b.build_load(Type::i64(), frame, BRONZE_ABI_SORT_FRAME_COUNT_OFFSET);
    Value* fn_slot = b.build_load(Type::i64(), frame, BRONZE_ABI_SORT_FRAME_FN_OFFSET);
    // Asked once: whether the comparator may be entered directly is a fact
    // about the function object, which the sort holds and no call changes.
    Value* direct = b.build_select(closure_enters_directly(b, b.build_load(Type::i64(), fn_slot, 0)), iconst(b, 1),
                                   iconst(b, 0));
    b.build_br(width_head, {iconst(b, 1), b.build_load(Type::i64(), frame, BRONZE_ABI_SORT_FRAME_SRC_OFFSET),
                            b.build_load(Type::i64(), frame, BRONZE_ABI_SORT_FRAME_DST_OFFSET)});

    // for (width = 1; width < count; width *= 2) { ...; swap(src, dst) }
    b.position_at_end(width_head);
    b.build_br_if(b.build_ult(width, count), run_head, {iconst(b, 0)}, done, {});

    // for (lo = 0; lo < count; lo += 2 * width) mergeRuns(lo, mid, hi)
    b.position_at_end(run_head);
    b.build_br_if(b.build_ult(lo, count), run_body, {}, width_next, {});

    b.position_at_end(run_body);
    Value* mid = umin(b, b.build_add(lo, width), count);
    Value* hi = umin(b, b.build_add(lo, b.build_add(width, width)), count);
    Value* eight = iconst(b, 8);
    Value* p_mid = b.build_add(src, b.build_mul(mid, eight));
    Value* p_hi = b.build_add(src, b.build_mul(hi, eight));
    b.build_br(merge_head, {b.build_add(src, b.build_mul(lo, eight)), p_mid, b.build_add(dst, b.build_mul(lo, eight))});

    // while (i < mid && j < hi): one comparison, one element out.
    b.position_at_end(merge_head);
    b.build_br_if(b.build_and(b.build_ult(pi, p_mid), b.build_ult(pj, p_hi)), merge_body, {}, left_head, {pi, pj, pk});

    b.position_at_end(merge_body);
    Value* x = b.build_load(Type::i64(), pi, 0);
    Value* y = b.build_load(Type::i64(), pj, 0);
    Value* x_undef = b.build_eq(x, iconst(b, static_cast<int64_t>(kUndefinedTag)));
    Value* y_undef = b.build_eq(y, iconst(b, static_cast<int64_t>(kUndefinedTag)));
    b.build_br_if(b.build_or(x_undef, y_undef), undef, call);

    // 23.1.3.30.2 steps 1-3, with no call: two undefineds are equal (left
    // first), an undefined x sorts after y, an undefined y after x.
    b.position_at_end(undef);
    b.build_br(pick, {b.build_select(y_undef, iconst(b, 1), b.build_select(x_undef, iconst(b, 0), iconst(b, 1)))});

    // Step 4: the comparator, called as compiled code calls a closure (the
    // slot re-read, as the collector may have moved it), its answer through
    // ToNumber; `v > 0` takes the right run, and NaN, no ordering, is equal.
    b.position_at_end(call);
    b.build_store(Type::i64(), frame, BRONZE_ABI_SORT_FRAME_PAIR_OFFSET, x);
    b.build_store(Type::i64(), frame, BRONZE_ABI_SORT_FRAME_PAIR_OFFSET + 8, y);
    Value* comparefn = b.build_load(Type::i64(), fn_slot, 0);
    Value* undefined = iconst(b, static_cast<int64_t>(kUndefinedTag));
    b.build_br_if(b.build_ne(direct, iconst(b, 0)), call_direct, call_dynamic);
    b.position_at_end(call_direct);
    b.build_br(answered, {enter_closure_directly(b, comparefn, undefined, 2, frame)});
    b.position_at_end(call_dynamic);
    b.build_br(answered, {b.build_call("bronze_dynamic_call", Type::i64(),
                                       {comparefn, undefined, b.build_iconst_i32(2), frame})});

    b.position_at_end(answered);
    Value* zero = b.build_fconst_f64(0.0);
    Value* is_number = b.build_ule(answer, iconst(b, static_cast<int64_t>(kBronzeNumberMaxBits)));
    Value* fast_right = b.build_sgt(b.build_bitcast_f64_i64(answer), zero);
    b.build_br_if(is_number, pick, {b.build_select(fast_right, iconst(b, 0), iconst(b, 1))}, to_number, {});

    b.position_at_end(to_number);
    Value* converted = b.build_call("bronze_unbox_f64", Type::f64(), {answer});
    b.build_br(pick, {b.build_select(b.build_sgt(converted, b.build_fconst_f64(0.0)), iconst(b, 0), iconst(b, 1))});

    // dst[k++] = take_left ? src[i++] : src[j++], read AFTER the comparison:
    // the call may have collected, and the slots are where the moved Values
    // are. A branch and not a select: the next comparison's loads then wait
    // on the prediction rather than on the comparator's answer. On an
    // already-ordered list the select form ran 19% more cycles than the C++
    // merge it replaces; on shuffled painter lists it was 6% ahead of this.
    b.position_at_end(pick);
    b.build_br_if(b.build_ne(take_left, iconst(b, 0)), take_l, take_r);
    b.position_at_end(take_l);
    b.build_store(Type::i64(), pk, 0, b.build_load(Type::i64(), pi, 0));
    b.build_br(merge_head, {b.build_add(pi, iconst(b, 8)), pj, b.build_add(pk, iconst(b, 8))});
    b.position_at_end(take_r);
    b.build_store(Type::i64(), pk, 0, b.build_load(Type::i64(), pj, 0));
    b.build_br(merge_head, {pi, b.build_add(pj, iconst(b, 8)), b.build_add(pk, iconst(b, 8))});

    // The rest of either run, in order.
    b.position_at_end(left_head);
    b.build_br_if(b.build_ult(li, p_mid), left_copy, {}, right_head, {lj, lk});
    b.position_at_end(left_copy);
    b.build_store(Type::i64(), lk, 0, b.build_load(Type::i64(), li, 0));
    b.build_br(left_head, {b.build_add(li, iconst(b, 8)), lj, b.build_add(lk, iconst(b, 8))});

    b.position_at_end(right_head);
    b.build_br_if(b.build_ult(rj, p_hi), right_copy, {}, run_next, {});
    b.position_at_end(right_copy);
    b.build_store(Type::i64(), rk, 0, b.build_load(Type::i64(), rj, 0));
    b.build_br(right_head, {b.build_add(rj, iconst(b, 8)), b.build_add(rk, iconst(b, 8))});

    b.position_at_end(run_next);
    b.build_br(run_head, {b.build_add(lo, b.build_add(width, width))});

    b.position_at_end(width_next);
    b.build_br(width_head, {b.build_add(width, width), dst, src});

    b.position_at_end(done);
    b.build_ret(src);
    fn->rebuild_cfg_predecessors();
    fn->sort_blocks_rpo();
    fn->rebuild_cfg_predecessors();
}

}  // namespace il2mir
