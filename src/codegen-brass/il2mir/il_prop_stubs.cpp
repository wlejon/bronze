// The property sites' out-of-line stubs (il_property.h): kPropGetSlowStub,
// the read's whole cached answer, and kPropSetStub, the write's. Built once
// per module by emit_direct_stubs (il_lowering_direct.cpp).
#include "il_direct_common.h"

#include "il_lowering.h"
#include "il_write_barrier.h"

#include <brass/mir/block.hpp>
#include <brass/mir/function.hpp>

#include <string>

namespace il2mir::direct {

namespace {

// The read stub's FUNCTION arm: a static of a function receiver from the
// site's ways, answered as rtFunctionCachedMember answers it (the runtime's
// copy, rt_prop_function.cpp, which says why each answer is the ladder's).
// The receiver must be a function the arms serve (rtStaticsArmServes: compiled
// from source, with a `prototype` property), and a way must name its statics
// box's shape with
//  - an own data slot of the box: the slot;
//  - an own accessor of the box: its getter, called with the FUNCTION as
//    `this`;
//  - a statics chain entry (BRONZE_ABI_IC_DEPTH_STATICS_FLAG) whose epoch is
//    the thread's, 1..kMaxStubDepth links up the box chain: the holder's slot
//    or getter.
// Anything else branches to `helper`. The builder enters in the block that
// would have called the helper and leaves every block of the arm terminated.
void build_fn_static_arm(PropertyLoweringHelper& pl, Builder& b, Value* tls, Value* obj, Value* site,
                         BasicBlock* helper) {
    const std::string p = "pgetfn";
    BasicBlock* cur = b.current_block();
    BasicBlock* head = b.append_block(p + "_head");
    BasicBlock* flags = b.append_block(p + "_flags");
    BasicBlock* box_at = b.append_block(p + "_box");
    BasicBlock* box_plain = b.append_block(p + "_boxplain");
    BasicBlock* hit = b.append_block(p + "_hit");
    Value* word = b.add_block_param(hit, Type::i64());
    Value* epoch = b.add_block_param(hit, Type::i64());
    BasicBlock* own = b.append_block(p + "_own");
    BasicBlock* not_own = b.append_block(p + "_notown");
    BasicBlock* chain = b.append_block(p + "_chain");
    BasicBlock* chain_data = b.append_block(p + "_chaindata");
    BasicBlock* getter = b.append_block(p + "_getter");
    Value* holder = b.add_block_param(getter, Type::i64());
    BasicBlock* enter = b.append_block(p + "_enter");

    b.position_at_end(cur);
    Value* ptr = b.build_xor(obj, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(ptr, b.build_iconst_i64(kPayloadLimit)), head, helper);

    b.position_at_end(head);
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kFunctionHeaderLow)), flags,
                  helper);

    b.position_at_end(flags);
    constexpr int64_t kServeMask = BRONZE_ABI_FN_FLAG_NATIVE | BRONZE_ABI_FN_FLAG_PROTOTYPE;
    Value* fn_flags = b.build_and(b.build_load(Type::i64(), ptr, BRONZE_ABI_FN_FLAGS_WORD_OFFSET),
                                  b.build_iconst_i64(kServeMask));
    b.build_br_if(b.build_eq(fn_flags, b.build_iconst_i64(BRONZE_ABI_FN_FLAG_PROTOTYPE)), box_at, helper);

    b.position_at_end(box_at);
    Value* box = b.build_load(Type::i64(), ptr, BRONZE_ABI_FN_PROPERTIES_OFFSET);
    Value* bptr = b.build_xor(box, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(bptr, b.build_iconst_i64(kPayloadLimit)), box_plain, helper);

    b.position_at_end(box_plain);
    BasicBlock* scan = b.append_block(p + "_scan");
    b.position_at_end(box_plain);
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), bptr, 0), b.build_iconst_i32(kPlainHeaderLow)), scan, helper);
    b.position_at_end(scan);
    PropEntry e;
    e.shape = b.build_load(Type::i64(), bptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    e.ptr = bptr;
    scan_ways(b, e, site, 0, BRONZE_ABI_IC_WAYS);
    b.build_br_if(e.hit, hit, {e.slot_word, e.epoch}, helper, {});

    b.position_at_end(hit);
    b.build_br_if(b.build_ult(word, b.build_iconst_i64(kOwnDataLimit)), own, not_own);

    b.position_at_end(own);
    b.build_ret(pl.load_slot(b, bptr, word));

    // The box's own accessor, or a statics chain entry while the epoch holds.
    b.position_at_end(not_own);
    Value* depth = b.build_lshr(word, b.build_iconst_i64(32));
    Value* slot = b.build_and(word, b.build_iconst_i64(kSlotMask));
    BasicBlock* statics = b.append_block(p + "_statics");
    b.position_at_end(not_own);
    b.build_br_if(b.build_eq(depth, b.build_iconst_i64(kAccessorDepth)), getter, {bptr}, statics, {});

    b.position_at_end(statics);
    Value* links = b.build_and(depth, b.build_iconst_i64(~(kStaticsDepth | kAccessorDepth)));
    Value* flagged = b.build_ne(b.build_and(depth, b.build_iconst_i64(kStaticsDepth)), b.build_iconst_i64(0));
    Value* in_reach = b.build_ult(b.build_sub(links, b.build_iconst_i64(1)), b.build_iconst_i64(kMaxStubDepth));
    Value* only = b.build_eq(b.build_and(depth, b.build_iconst_i64(~(kStaticsDepth | kAccessorDepth | 0xFFFF))),
                             b.build_iconst_i64(0));
    Value* current = b.build_eq(epoch, b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF));
    b.build_br_if(b.build_and(b.build_and(flagged, only), b.build_and(in_reach, current)), chain, helper);

    b.position_at_end(chain);
    Value* pptr = walk_to_prototype(b, e.shape, b.build_and(depth, b.build_iconst_i64(~kStaticsDepth)), helper, p);
    b.build_br_if(b.build_ne(b.build_and(depth, b.build_iconst_i64(kAccessorDepth)), b.build_iconst_i64(0)), getter,
                  {pptr}, chain_data, {});

    b.position_at_end(chain_data);
    b.build_ret(pl.load_slot(b, pptr, slot));

    // A getter sees the function, not the box it is kept in.
    b.position_at_end(getter);
    Value* fn_val = pl.load_slot(b, holder, slot);
    const EntryTest t = entry_test_br(b, tls, fn_val, kCallMask, kCallBits, helper);
    b.build_br_if(t.ok, enter, helper);

    b.position_at_end(enter);
    Value* code = b.build_load(Type::ptr(), t.fn, BRONZE_ABI_FN_CODE_OFFSET);
    Value* env = b.build_load(Type::i64(), t.fn, BRONZE_ABI_FN_ENV_OFFSET);
    b.build_ret(call_code(b, code, env, obj, 0u, b.build_iconst_i64(0)));
}

}  // namespace

// The full read stub: kPropGetSlowStub(obj, key, site), bronze_prop_get's
// operands, called by kPropGetStub (il_prop_get_fast.cpp) for what its hit
// paths do not answer.
// It answers what propGetHelperBody's site-cache path answers, test for
// test, from any of the site's ways:
//  - an own data slot (the inline paths' hit, for a site without them);
//  - the negative entry: undefined, while the epoch holds;
//  - an entry on the prototype chain, up to kMaxStubDepth links (a class's
//    methods and accessors, an inherited constant): its data slot, or its
//    getter;
//  - a depth-0 accessor (one defined on the receiver itself): its getter.
// A getter is entered directly when compiled code may enter it
// (BRONZE_ABI_FN_FLAG_DIRECT_CALL), with the receiver as `this` and no
// arguments, which is the call the helper makes through rtEnterJs; anything
// else, and every receiver no way describes, is the helper. The stub holds no
// reference across a call and names no frame in a stack trace.
void build_prop_get_stub(IlLowering* lowering, Function* fn) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* cur = b.append_block("entry");
    Value* obj = b.add_block_param(cur, Type::i64());
    Value* key_id = b.add_block_param(cur, Type::i32());
    Value* site = b.add_block_param(cur, Type::ptr());
    const std::string prefix = "pget";
    BasicBlock* own = b.append_block(prefix + "_own");
    BasicBlock* not_own = b.append_block(prefix + "_notown");
    BasicBlock* absent = b.append_block(prefix + "_absent");
    BasicBlock* not_absent = b.append_block(prefix + "_notabsent");
    BasicBlock* depth1 = b.append_block(prefix + "_depth1");
    BasicBlock* not_depth1 = b.append_block(prefix + "_notdepth1");
    BasicBlock* proto_data = b.append_block(prefix + "_protodata");
    BasicBlock* getter = b.append_block(prefix + "_getter");
    Value* holder = b.add_block_param(getter, Type::i64());
    BasicBlock* enter = b.append_block(prefix + "_enter");
    BasicBlock* slow = b.append_block(prefix + "_slow");

    BasicBlock* mega = b.append_block(prefix + "_mega");
    BasicBlock* hit = b.append_block(prefix + "_hit");
    Value* slot_word = b.add_block_param(hit, Type::i64());
    Value* entry_epoch = b.add_block_param(hit, Type::i64());

    BasicBlock* rest = b.append_block(prefix + "_rest");

    // Way 0 first, on a branch of its own: a monomorphic site whose inline
    // read cannot answer (a prototype member, an absent key, a getter) hits
    // it on every call, and the select scan over all four ways was a third
    // of the stub for it. Only a receiver way 0 does not name pays the scan.
    b.position_at_end(cur);
    Value* tls = tls_block(b);
    PropEntry e;
    keyed_receiver(pl, b, obj, e);
    {
        Value* match0 = b.build_eq(e.shape, b.build_load(Type::i64(), site, BRONZE_ABI_IC_SHAPE_OFFSET));
        Value* word0 = b.build_load(Type::i64(), site, BRONZE_ABI_IC_SLOT_OFFSET);
        Value* epoch0 = b.build_load(Type::i64(), site, BRONZE_ABI_IC_EPOCH_OFFSET);
        b.build_br_if(match0, hit, {word0, epoch0}, rest, {});
    }

    b.position_at_end(rest);
    scan_ways(b, e, site, 1, BRONZE_ABI_IC_WAYS);
    b.build_br_if(e.hit, hit, {e.slot_word, e.epoch}, mega, {});

    // Every way missed: the thread's (shape, key) table.
    b.position_at_end(mega);
    build_mega_probe(b, tls, e, site, BRONZE_ABI_IC_WAYS, b.build_zext_i64(key_id), hit, slow);

    b.position_at_end(hit);
    Value* own_data = b.build_ult(slot_word, b.build_iconst_i64(kOwnDataLimit));
    b.build_br_if(own_data, own, not_own);

    b.position_at_end(own);
    b.build_ret(pl.load_slot_br(b, e.ptr, slot_word));

    b.position_at_end(not_own);
    Value* depth = b.build_lshr(slot_word, b.build_iconst_i64(32));
    Value* slot = b.build_and(slot_word, b.build_iconst_i64(kSlotMask));
    Value* current = b.build_eq(entry_epoch, b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF));
    b.build_br_if(b.build_and(current, b.build_eq(depth, b.build_iconst_i64(BRONZE_ABI_IC_DEPTH_ABSENT_FLAG))),
                  absent, not_absent);

    b.position_at_end(absent);
    b.build_ret(b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag)));

    // A chain entry with no flag but the accessor's.
    b.position_at_end(not_absent);
    Value* d1 = walkable_depth(b, depth);
    b.build_br_if(b.build_and(current, d1), depth1, not_depth1);

    b.position_at_end(depth1);
    Value* pptr = walk_to_prototype(b, e.shape, depth, slow, prefix);
    b.build_br_if(b.build_ne(b.build_and(depth, b.build_iconst_i64(kAccessorDepth)), b.build_iconst_i64(0)), getter,
                  {pptr}, proto_data, {});

    b.position_at_end(proto_data);
    b.build_ret(pl.load_slot(b, pptr, slot));

    b.position_at_end(not_depth1);
    b.build_br_if(b.build_eq(depth, b.build_iconst_i64(kAccessorDepth)), getter, {e.ptr}, slow, {});

    b.position_at_end(getter);
    Value* fn_val = pl.load_slot(b, holder, slot);
    const EntryTest t = entry_test_br(b, tls, fn_val, kCallMask, kCallBits, slow);
    b.build_br_if(t.ok, enter, slow);

    b.position_at_end(enter);
    Value* code = b.build_load(Type::ptr(), t.fn, BRONZE_ABI_FN_CODE_OFFSET);
    Value* env = b.build_load(Type::i64(), t.fn, BRONZE_ABI_FN_ENV_OFFSET);
    b.build_ret(call_code(b, code, env, obj, 0u, b.build_iconst_i64(0)));

    BasicBlock* helper = b.append_block(prefix + "_helper");
    b.position_at_end(slow);
    build_fn_static_arm(pl, b, tls, obj, site, helper);
    b.position_at_end(helper);
    b.build_ret(b.build_call("bronze_prop_get", Type::i64(), {obj, key_id, site}));
    fn->rebuild_cfg_predecessors();
}

// The write stub: kPropSetStub(obj, key, val, site, strict), bronze_prop_set's
// operands. A write site uses way 0 only (bronze_abi.h), and the stub answers
// from it what the helper's site-cache path answers:
//  - an own data slot: the store lower_prop_set_mono makes, with the barrier
//    for a reference (the fill names only writable own data, and an f64 slot
//    carries a flag that refuses it here);
//  - an accessor on the receiver or on its prototype chain (to
//    kMaxStubDepth links): its setter, entered
//    directly when it may be, with the receiver as `this` and the value as
//    its one argument, staged in a traced slot of this frame — the call the
//    helper makes through rtEnterJs;
//  - an add whose transition way 0 holds (emit_transition_arm's tests, the
//    same as a constructor's inline arm): the new shape and the value.
// Everything else is the helper.
void build_prop_set_stub(IlLowering* lowering, Function* fn) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* cur = b.append_block("entry");
    Value* obj = b.add_block_param(cur, Type::i64());
    Value* key_id = b.add_block_param(cur, Type::i32());
    Value* val = b.add_block_param(cur, Type::i64());
    Value* site = b.add_block_param(cur, Type::ptr());
    Value* strict = b.add_block_param(cur, Type::i32());
    const std::string prefix = "pset";
    BasicBlock* own = b.append_block(prefix + "_own");
    BasicBlock* barrier = b.append_block(prefix + "_barrier");
    BasicBlock* done = b.append_block(prefix + "_done");
    BasicBlock* not_own = b.append_block(prefix + "_notown");
    BasicBlock* depth1 = b.append_block(prefix + "_depth1");
    BasicBlock* not_depth1 = b.append_block(prefix + "_notdepth1");
    BasicBlock* setter = b.append_block(prefix + "_setter");
    Value* holder = b.add_block_param(setter, Type::i64());
    BasicBlock* enter = b.append_block(prefix + "_enter");
    BasicBlock* slow = b.append_block(prefix + "_slow");

    BasicBlock* mega = b.append_block(prefix + "_mega");
    BasicBlock* hit = b.append_block(prefix + "_hit");
    Value* slot_word = b.add_block_param(hit, Type::i64());
    Value* entry_epoch = b.add_block_param(hit, Type::i64());

    b.position_at_end(cur);
    Value* argv = b.build_alloca_tagged(1);
    Value* tls = tls_block(b);
    const PropEntry e = prop_entry(pl, b, obj, site, 1);
    BasicBlock* scanned = b.current_block();
    BasicBlock* transition = b.append_block(prefix + "_transition");
    b.position_at_end(scanned);
    b.build_br_if(e.hit, hit, {e.slot_word, e.epoch}, transition, {});

    // Way 0 names the shape one add of this key above the receiver's: the
    // add, as a constructor's inline arm makes it (emit_transition_arm, which
    // says what each of its tests is for), for every site that reaches the
    // stub. Without it each add was the helper's whole store: a listener
    // object made by a plain function constructor, a field a setter adds.
    b.position_at_end(transition);
    {
        Value* plain = nullptr;
        Value* ptr = nullptr;
        Value* shape = nullptr;
        pl.receiver_shape(b, obj, plain, ptr, shape);
        IcSite way0;
        way0.base = site;
        way0.off = 0;
        way0.packed = false;
        pl.emit_transition_arm(b, ptr, shape, plain, way0, val, /*need_barrier=*/true, done, prefix);
        b.build_br(mega);
    }

    // Way 0 missed: the thread's table, in its write key space. A table
    // answer latches nothing, so a site whose way 0 holds a shape that stopped
    // coming kept missing it for good (three_skinned: ~2-5K table answers a
    // frame at sites whose way 0 named a shape no frame wrote); every
    // BRONZE_ABI_IC_SET_MEGA_RELATCH-th answer goes to the helper instead,
    // which skips the table for it and so stores through way 0, whose fill puts
    // the receiver's shape there (and in its row).
    b.position_at_end(mega);
    {
        static_assert(BRONZE_ABI_IC_SET_MEGA_COUNT_WORD * kWord >= BRONZE_ABI_IC_ENTRY_SIZE &&
                          BRONZE_ABI_IC_SET_MEGA_COUNT_WORD * kWord < BRONZE_ABI_IC_SITE_SIZE,
                      "the count is a word of the site past the way a write site uses");
        BasicBlock* probe = b.append_block(prefix + "_megaprobe");
        b.position_at_end(mega);
        Value* n = b.build_add(b.build_load(Type::i64(), site, BRONZE_ABI_IC_SET_MEGA_COUNT_WORD * kWord),
                               b.build_iconst_i64(1));
        Value* relatch = b.build_uge(n, b.build_iconst_i64(BRONZE_ABI_IC_SET_MEGA_RELATCH));
        // The helper sees the count at the threshold, takes the store past
        // the table and back to zero.
        b.build_store(Type::i64(), site, BRONZE_ABI_IC_SET_MEGA_COUNT_WORD * kWord, n);
        b.build_br_if(relatch, slow, probe);
        b.position_at_end(probe);
        build_mega_probe(b, tls, e, site, 1,
                         b.build_or(b.build_zext_i64(key_id),
                                    b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_MEGA_PROP_SET_KEY_BIT))),
                         hit, slow);
    }

    b.position_at_end(hit);
    Value* own_data = b.build_ult(slot_word, b.build_iconst_i64(kOwnDataLimit));
    b.build_br_if(own_data, own, not_own);

    b.position_at_end(own);
    Value* base = pl.slot_base(b, e.ptr, slot_word);
    Value* slot_addr = b.build_add(b.build_add(base, b.build_shl(slot_word, b.build_iconst_i64(3))),
                                   b.build_iconst_i64(kBronzeObjSlotsOffset));
    b.build_store(Type::i64(), slot_addr, 0, val);
    Value* is_ref = b.build_ugt(val, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
    b.build_br_if(is_ref, barrier, done);

    b.position_at_end(barrier);
    build_write_barrier(b, slot_addr, val);
    b.build_br(done);

    b.position_at_end(done);
    b.build_ret_void();

    b.position_at_end(not_own);
    Value* depth = b.build_lshr(slot_word, b.build_iconst_i64(32));
    Value* slot = b.build_and(slot_word, b.build_iconst_i64(kSlotMask));
    Value* current = b.build_eq(entry_epoch, b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF));
    b.build_br_if(b.build_and(current,
                              b.build_and(walkable_depth(b, depth),
                                          b.build_ne(b.build_and(depth, b.build_iconst_i64(kAccessorDepth)),
                                                     b.build_iconst_i64(0)))),
                  depth1, not_depth1);

    b.position_at_end(depth1);
    Value* pptr = walk_to_prototype(b, e.shape, depth, slow, prefix);
    b.build_br(setter, {pptr});

    b.position_at_end(not_depth1);
    b.build_br_if(b.build_eq(depth, b.build_iconst_i64(kAccessorDepth)), setter, {e.ptr}, slow, {});

    // An accessor's setter is the slot after its getter.
    b.position_at_end(setter);
    Value* fn_val = pl.load_slot(b, holder, b.build_add(slot, b.build_iconst_i64(1)));
    const EntryTest t = entry_test_br(b, tls, fn_val, kCallMask, kCallBits, slow);
    b.build_br_if(t.ok, enter, slow);

    b.position_at_end(enter);
    Value* code = b.build_load(Type::ptr(), t.fn, BRONZE_ABI_FN_CODE_OFFSET);
    Value* env = b.build_load(Type::i64(), t.fn, BRONZE_ABI_FN_ENV_OFFSET);
    b.build_store(Type::i64(), argv, 0, val);
    call_code(b, code, env, obj, 1u, argv);
    b.build_ret_void();

    b.position_at_end(slow);
    b.build_call("bronze_prop_set", Type::void_type(), {obj, key_id, val, site, strict});
    b.build_ret_void();
    fn->rebuild_cfg_predecessors();
}

}  // namespace il2mir::direct
