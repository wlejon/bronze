// Direct entry: a call from compiled code to a compiled callee that loads the
// callee's code and env words and calls them, the way the runtime's own
// dispatch ends, without the runtime in between.
//
// What the runtime path does and why a direct entry may skip it is stated
// once, at BRONZE_ABI_FN_FLAG_DIRECT_CALL (bronze_abi.h). Every arm below
// tests exactly what that argument needs of the callee, against the live
// function object or against what the method IC's latch recorded of it, and
// sends everything else to the helper the site would have called anyway —
// which is always correct, so a test that refuses too much costs speed and
// never an answer.
#include "il_lowering_direct.h"
#include "il_direct_common.h"

#include "il_abi.h"
#include "il_array_sort.h"
#include "il_lowering.h"
#include "il_prop_get_fast.h"
#include "il_write_barrier.h"

#include <string>

namespace il2mir {

using namespace direct;

namespace {

constexpr const char* kMethodCallStub = "__bronze_method_call_ic";

}  // namespace

bool direct_entry_here(const IlLowering* lowering) {
    return lowering && lowering->options().direct_js_calls && lowering->options().pin_tls_register &&
           !lowering->run_once() && !lowering->in_protected_block();
}

bool direct_method_ic_here(const IlLowering* lowering) {
    return lowering && lowering->options().direct_js_calls && lowering->options().pin_tls_register;
}

Value* lower_closure_call(IlLowering* lowering, Builder& b, Value* callee, Value* this_val,
                          const std::vector<Value*>& boxed_args) {
    const uint32_t argc = static_cast<uint32_t>(boxed_args.size());
    return lower_closure_call_argv(b, callee, this_val, argc, lowering->stage_argv(b, boxed_args));
}

Value* closure_enters_directly(Builder& b, Value* callee) {
    return entry_test(b, tls_block(b), callee, kCallMask, kCallBits).ok;
}

Value* enter_closure_directly(Builder& b, Value* callee, Value* this_val, uint32_t argc, Value* argv) {
    Value* fn = b.build_xor(callee, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    Value* code = b.build_load(Type::ptr(), fn, BRONZE_ABI_FN_CODE_OFFSET);
    Value* env = b.build_load(Type::i64(), fn, BRONZE_ABI_FN_ENV_OFFSET);
    return call_code(b, code, env, this_val, argc, argv);
}

Value* lower_closure_call_argv(Builder& b, Value* callee, Value* this_val, uint32_t argc, Value* argv) {
    const std::string prefix = block_prefix(b, "fcall");
    BasicBlock* cur = b.current_block();
    BasicBlock* enter = b.append_block(prefix + "_enter");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    Value* tls = tls_block(b);
    const EntryTest t = entry_test_br(b, tls, callee, kCallMask, kCallBits, slow);
    b.build_br_if(t.ok, enter, slow);

    b.position_at_end(enter);
    Value* code = b.build_load(Type::ptr(), t.fn, BRONZE_ABI_FN_CODE_OFFSET);
    Value* env = b.build_load(Type::i64(), t.fn, BRONZE_ABI_FN_ENV_OFFSET);
    b.build_br(merge, {call_code(b, code, env, this_val, argc, argv)});

    b.position_at_end(slow);
    b.build_br(merge, {b.build_call("bronze_dynamic_call", Type::i64(),
                                    {callee, this_val, b.build_iconst_i32(static_cast<int32_t>(argc)), argv})});

    b.position_at_end(merge);
    return result;
}

namespace {

// The method IC's dispatch, built once per module as a function of its own
// and called from every site with the operands bronze_call_method takes:
// inline at each site it would be some sixty instructions, which a program of
// thousands of method calls pays for in build time and code size. Being a
// direct call, it is also an invoke in a block with a handler, which an
// indirect call cannot be. It holds no reference across a call (each operand
// is the caller's, rooted there), and it names no function in a stack trace
// (it has no descriptor; stack_trace.cpp skips such frames).
void build_method_call_stub(IlLowering* lowering, Function* fn) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* cur = b.append_block("entry");
    Value* recv = b.add_block_param(cur, Type::i64());
    Value* key_id = b.add_block_param(cur, Type::i32());
    Value* argc = b.add_block_param(cur, Type::i32());
    Value* argv = b.add_block_param(cur, Type::ptr());
    Value* site = b.add_block_param(cur, Type::ptr());
    // The site's method row (bronze_abi_layout.h, "the METHOD ROWS"), or
    // zero: the way-1 swap writes way 0's new guard words there too.
    Value* row = b.add_block_param(cur, Type::i64());
    const std::string prefix = "mcall";
    BasicBlock* way0 = b.append_block(prefix + "_way0");
    BasicBlock* direct0 = b.append_block(prefix + "_direct0");
    BasicBlock* slot = b.append_block(prefix + "_slot");
    BasicBlock* not_way0 = b.append_block(prefix + "_notway0");
    BasicBlock* mega = b.append_block(prefix + "_mega");
    BasicBlock* mega_hit = b.append_block(prefix + "_megahit");
    Value* mega_word = b.add_block_param(mega_hit, Type::i64());
    Value* mega_epoch = b.add_block_param(mega_hit, Type::i64());
    BasicBlock* mega_own = b.append_block(prefix + "_megaown");
    BasicBlock* mega_chain = b.append_block(prefix + "_megachain");
    BasicBlock* mega_callee = b.append_block(prefix + "_megacallee");
    Value* mega_fn = b.add_block_param(mega_callee, Type::i64());
    BasicBlock* exotic = b.append_block(prefix + "_exotic");
    BasicBlock* exotic_box = b.append_block(prefix + "_exotic_box");
    BasicBlock* exotic_code = b.append_block(prefix + "_exotic_code");
    BasicBlock* fn_recv = b.append_block(prefix + "_fnrecv");
    BasicBlock* enter = b.append_block(prefix + "_enter");
    Value* enter_code = b.add_block_param(enter, Type::ptr());
    Value* enter_env = b.add_block_param(enter, Type::i64());
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    Value* tls = tls_block(b);
    PropEntry re;
    keyed_receiver(pl, b, recv, re);
    Value* ptr = re.ptr;
    Value* shape = re.shape;
    Value* w0 = b.build_load(Type::i64(), site, 0);
    Value* arity0 = b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_ARITY_WORD * kWord);
    Value* direct_bit = b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_METHOD_IC_DIRECT_ENTRY_BIT));
    Value* zero = b.build_iconst_i64(0);
    b.build_br_if(b.build_eq(shape, w0), way0, not_way0);

    // Way 0 names this receiver's shape: the SLOT form when the arity word's
    // high half carries a slot, else the DIRECT form.
    b.position_at_end(way0);
    b.build_br_if(b.build_uge(arity0, b.build_iconst_i64(kSlotFormLimit)), slot, direct0);

    // DIRECT: the latched code and env, while the epoch it was latched at is
    // current (a patched or shadowed method moves it).
    b.position_at_end(direct0);
    {
        Value* epoch = b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF);
        Value* ok = b.build_and(
            b.build_ne(b.build_and(arity0, direct_bit), zero),
            b.build_eq(b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_EPOCH_WORD * kWord), epoch));
        Value* code = b.build_load(Type::ptr(), site, BRONZE_ABI_METHOD_IC_CODE_WORD * kWord);
        Value* env = b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_ENV_WORD * kWord);
        b.build_br_if(ok, enter, {code, env}, slow, {});
    }

    // SLOT: the receiver's own slot holds the callee, which is re-read and
    // re-tested at every call — same-shape receivers may hold different
    // functions there.
    b.position_at_end(slot);
    {
        Value* slot_word = b.build_sub(b.build_lshr(arity0, b.build_iconst_i64(BRONZE_ABI_METHOD_IC_SLOT_SHIFT)),
                                       b.build_iconst_i64(1));
        Value* callee = pl.load_slot_br(b, ptr, slot_word);
        const EntryTest t = entry_test_br(b, tls, callee, kCallMask, kCallBits, slow);
        Value* code = b.build_load(Type::ptr(), t.fn, BRONZE_ABI_FN_CODE_OFFSET);
        Value* env = b.build_load(Type::i64(), t.fn, BRONZE_ABI_FN_ENV_OFFSET);
        b.build_br_if(t.ok, enter, {code, env}, slow, {});
    }

    // Way 1 holds a plain-receiver DIRECT entry and nothing else.
    b.position_at_end(not_way0);
    {
        constexpr int32_t w1 = BRONZE_ABI_METHOD_IC_WAY1_SHAPE_WORD;
        Value* arity1 = b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_WAY1_ARITY_WORD * kWord);
        Value* epoch = b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF);
        Value* hit = b.build_and(
            b.build_eq(shape, b.build_load(Type::i64(), site, w1 * kWord)),
            b.build_and(
                b.build_and(b.build_ne(b.build_and(arity1, direct_bit), zero),
                            b.build_ult(arity1, b.build_iconst_i64(kSlotFormLimit))),
                b.build_eq(b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_WAY1_EPOCH_WORD * kWord), epoch)));
        BasicBlock* front = b.append_block(prefix + "_way1front");
        b.position_at_end(not_way0);
        b.build_br_if(hit, front, mega);
        b.position_at_end(front);
        Value* code = b.build_load(Type::ptr(), site, BRONZE_ABI_METHOD_IC_WAY1_CODE_WORD * kWord);
        Value* env = b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD * kWord);
        build_method_way1_front(b, site, row, tls, code, env, enter);
    }

    // Both ways missed: the thread's (shape, key) read table, which the
    // runtime's method lookup fills (bronze_call_method reads the method
    // through bronze_prop_get). A site whose receivers rotate through more
    // shapes than its two ways — `object.onBeforeRender(...)` over a scene's
    // Mesh, Line, Points and Sprite — otherwise took the by-name lookup on
    // every call. Only a full site probes (way 1 latched), for the reason
    // build_mega_probe states. The entry is a read entry and is read as the
    // read stub reads one: an own data slot, or a data slot up the chain
    // while its epoch is current; the callee then takes the SLOT form's
    // test, since nothing about it was latched here.
    //
    // A table answer latches nothing, so every
    // BRONZE_ABI_METHOD_IC_MEGA_RELATCH-th one at a full site goes to the
    // helper instead, whose latch moves the receiver into way 0
    // (BRONZE_ABI_METHOD_IC_MEGA_COUNT_WORD says why).
    b.position_at_end(mega);
    {
        const PropEntry& e = re;
        static_assert(BRONZE_ABI_METHOD_IC_MEGA_COUNT_WORD * kWord < BRONZE_ABI_IC_SITE_SIZE,
                      "the table-answer count is a word of the site");
        Value* full = b.build_ne(b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_WAY1_SHAPE_WORD * kWord), zero);
        BasicBlock* count = b.append_block(prefix + "_megacount");
        BasicBlock* probe = b.append_block(prefix + "_megaprobe");
        b.position_at_end(mega);
        b.build_br_if(e.plain ? b.build_and(e.plain, full) : full, count, exotic);
        b.position_at_end(count);
        Value* n = b.build_add(b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_MEGA_COUNT_WORD * kWord),
                               b.build_iconst_i64(1));
        Value* relatch = b.build_uge(n, b.build_iconst_i64(BRONZE_ABI_METHOD_IC_MEGA_RELATCH));
        b.build_store(Type::i64(), site, BRONZE_ABI_METHOD_IC_MEGA_COUNT_WORD * kWord,
                      b.build_select(relatch, zero, n));
        b.build_br_if(relatch, slow, {}, probe, {});
        b.position_at_end(probe);
        build_mega_probe_if(b, tls, e, full, b.build_zext_i64(key_id), mega_hit, exotic);
    }
    b.position_at_end(mega_hit);
    b.build_br_if(b.build_ult(mega_word, b.build_iconst_i64(kOwnDataLimit)), mega_own, mega_chain);
    b.position_at_end(mega_own);
    b.build_br(mega_callee, {pl.load_slot(b, ptr, mega_word)});
    b.position_at_end(mega_chain);
    {
        Value* depth = b.build_lshr(mega_word, b.build_iconst_i64(32));
        Value* current = b.build_eq(mega_epoch, b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF));
        // A data entry up the chain: walkable and no accessor flag.
        Value* data = b.build_eq(b.build_and(depth, b.build_iconst_i64(kAccessorDepth)), zero);
        BasicBlock* walk = b.append_block(prefix + "_megawalk");
        b.position_at_end(mega_chain);
        b.build_br_if(b.build_and(current, b.build_and(data, walkable_depth(b, depth))), walk, slow);
        b.position_at_end(walk);
        Value* holder = walk_to_prototype(b, shape, depth, slow, prefix + "_mega");
        b.build_br(mega_callee, {pl.load_slot(b, holder, b.build_and(mega_word, b.build_iconst_i64(kSlotMask)))});
    }
    b.position_at_end(mega_callee);
    {
        const EntryTest t = entry_test_br(b, tls, mega_fn, kCallMask, kCallBits, slow);
        Value* code = b.build_load(Type::ptr(), t.fn, BRONZE_ABI_FN_CODE_OFFSET);
        Value* env = b.build_load(Type::i64(), t.fn, BRONZE_ABI_FN_ENV_OFFSET);
        b.build_br_if(t.ok, enter, {code, env}, slow, {});
    }

    // EXOTIC with the BOX guard (an Array's C-table method): the receiver's
    // kind is the latched one and its named-property box holds no object.
    // The box offset comes out of word 0, so it is read only once word 0 is
    // known to be that encoding.
    b.position_at_end(exotic);
    {
        Value* form = b.build_and(w0, b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_METHOD_IC_EXOTIC_BIT |
                                                                              BRONZE_ABI_METHOD_IC_CODE_GUARD_BIT)));
        Value* box_form = b.build_eq(form, b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_METHOD_IC_EXOTIC_BIT)));
        // The tag asked of the receiver's own bits, not of `ptr`: the same
        // compare as the entry's object test would be one value, kept live
        // from the entry as a flag (a setcc and a test on every call).
        Value* is_obj = b.build_eq(b.build_lshr(recv, b.build_iconst_i64(48)),
                                   b.build_iconst_i64(BRONZE_ABI_TAG_OBJECT));
        Value* base = b.build_select(is_obj, ptr, tls);
        Value* kind = b.build_lshr(b.build_and(w0, b.build_iconst_i64(0xFFFFFFFFLL)),
                                   b.build_iconst_i64(BRONZE_ABI_METHOD_IC_KIND_SHIFT));
        Value* want_low = b.build_or(b.build_shl(kind, b.build_iconst_i64(16)), b.build_iconst_i64(BRONZE_ABI_TAG_OBJECT));
        Value* kind_ok = b.build_eq(b.build_zext_i64(b.build_load(Type::i32(), base, 0)), want_low);
        Value* entry_ok = b.build_and(b.build_ne(b.build_and(arity0, direct_bit), zero),
                                      b.build_ult(arity0, b.build_iconst_i64(kSlotFormLimit)));
        Value* code_form = b.build_eq(form, b.build_iconst_i64(static_cast<int64_t>(
                                                 BRONZE_ABI_METHOD_IC_EXOTIC_BIT | BRONZE_ABI_METHOD_IC_CODE_GUARD_BIT)));
        Value* latched = b.build_and(is_obj, b.build_and(kind_ok, entry_ok));
        BasicBlock* not_box = b.append_block(prefix + "_exotic_notbox");
        b.position_at_end(exotic);
        b.build_br_if(b.build_and(box_form, latched), exotic_box, not_box);
        b.position_at_end(not_box);
        b.build_br_if(b.build_and(code_form, latched), exotic_code, fn_recv);
    }

    // A FUNCTION receiver's SLOT entry (latchFunctionStaticsMethodIc): way 0
    // names the shape of the receiver's statics box and the slot holding the
    // callee, `Quaternion.slerpFlat(...)` on a class. Answered as methodIcHit
    // answers it, a plain box of that shape holds the key at that slot, and
    // the callee read there is entered as the SLOT arm enters one, where the
    // runtime would have taken bronze_dynamic_call's C++ entry. A site whose
    // receiver names a function binding makes these tests itself, first
    // (lower_method_fn_recv); this arm serves the rest (a parameter holding a
    // class, `this.constructor`).
    b.position_at_end(fn_recv);
    {
        BasicBlock* fobj = b.append_block(prefix + "_fnrecv_obj");
        BasicBlock* fbox = b.append_block(prefix + "_fnrecv_box");
        BasicBlock* fshape = b.append_block(prefix + "_fnrecv_shape");
        BasicBlock* fslot = b.append_block(prefix + "_fnrecv_slot");
        b.position_at_end(fn_recv);
        Value* is_obj = b.build_eq(b.build_lshr(recv, b.build_iconst_i64(48)),
                                   b.build_iconst_i64(BRONZE_ABI_TAG_OBJECT));
        b.build_br_if(b.build_and(is_obj, b.build_uge(arity0, b.build_iconst_i64(kSlotFormLimit))), fobj, slow);
        b.position_at_end(fobj);
        b.build_br_if(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kFunctionHeaderLow)), fbox,
                      slow);
        b.position_at_end(fbox);
        Value* box = b.build_load(Type::i64(), ptr, BRONZE_ABI_FN_PROPERTIES_OFFSET);
        Value* box_ptr = b.build_xor(box, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
        b.build_br_if(b.build_ult(box_ptr, b.build_iconst_i64(kPayloadLimit)), fshape, slow);
        b.position_at_end(fshape);
        Value* box_ok = b.build_and(
            b.build_eq(b.build_load(Type::i32(), box_ptr, 0), b.build_iconst_i32(kPlainHeaderLow)),
            b.build_eq(b.build_load(Type::i64(), box_ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET), w0));
        b.build_br_if(box_ok, fslot, slow);
        b.position_at_end(fslot);
        Value* slot_word = b.build_sub(b.build_lshr(arity0, b.build_iconst_i64(BRONZE_ABI_METHOD_IC_SLOT_SHIFT)),
                                       b.build_iconst_i64(1));
        Value* callee = pl.load_slot_br(b, box_ptr, slot_word);
        const EntryTest t = entry_test_br(b, tls, callee, kCallMask, kCallBits, slow);
        Value* code = b.build_load(Type::ptr(), t.fn, BRONZE_ABI_FN_CODE_OFFSET);
        Value* env = b.build_load(Type::i64(), t.fn, BRONZE_ABI_FN_ENV_OFFSET);
        b.build_br_if(t.ok, enter, {code, env}, slow, {});
    }
    b.position_at_end(exotic_box);
    {
        Value* box_at = b.build_add(ptr, b.build_lshr(w0, b.build_iconst_i64(BRONZE_ABI_METHOD_IC_BOX_SHIFT)));
        Value* box = b.build_load(Type::i64(), box_at, 0);
        Value* box_is_obj = b.build_ult(b.build_xor(box, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits))),
                                        b.build_iconst_i64(kPayloadLimit));
        Value* code = b.build_load(Type::ptr(), site, BRONZE_ABI_METHOD_IC_CODE_WORD * kWord);
        Value* env = b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag));
        b.build_br_if(box_is_obj, slow, {}, enter, {code, env});
    }

    // EXOTIC with the CODE guard: a typed-array view (its shape word against
    // the latched Shape*, `view.set(...)`) or a global constructor's static
    // (its code word, `Array.isArray(...)`). Entered here only while the
    // entry's epoch is the thread's: a view's entry always carries one, and
    // a constructor's carries one once its statics live in a box the program
    // may write (rt_method_call.cpp, latchExoticMethodIc); one that carries
    // none (zero, never an epoch) is the runtime's to answer. A native is
    // entered without its padding only with at least its arity in arguments,
    // as methodIcHit's `native` arm enters it.
    b.position_at_end(exotic_code);
    {
        Value* aux_at = b.build_add(ptr, b.build_lshr(w0, b.build_iconst_i64(BRONZE_ABI_METHOD_IC_BOX_SHIFT)));
        Value* aux_ok = b.build_eq(b.build_load(Type::i64(), aux_at, 0),
                                   b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_AUX_WORD * kWord));
        Value* epoch_ok = b.build_eq(b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_EPOCH_WORD * kWord),
                                     b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF));
        Value* arity = b.build_and(arity0, b.build_iconst_i64(0x7FFFFFFFLL));
        Value* args_ok = b.build_ule(arity, b.build_zext_i64(argc));
        Value* code = b.build_load(Type::ptr(), site, BRONZE_ABI_METHOD_IC_CODE_WORD * kWord);
        Value* env = b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag));
        b.build_br_if(b.build_and(aux_ok, b.build_and(epoch_ok, args_ok)), enter, {code, env}, slow, {});
    }

    b.position_at_end(enter);
    b.build_br(merge, {call_code(b, enter_code, enter_env, recv, argc, argv)});

    b.position_at_end(slow);
    b.build_br(merge, {b.build_call("bronze_call_method", Type::i64(),
                                    {recv, key_id, argc, argv, site})});

    b.position_at_end(merge);
    b.build_ret(result);
    fn->rebuild_cfg_predecessors();
}

}  // namespace

void emit_direct_stubs(IlLowering* lowering, Module& mod) {
    const bool here = direct_method_ic_here(lowering);
    lowering->prop_lowering().set_ic_stubs(here);
    if (!here || mod.get_function(kMethodCallStub)) return;
    // Built before any body: a function built while another is being built
    // shares the module's builder state with it.
    Function* stub = mod.create_function(kMethodCallStub, Type::i64(),
                                         {Type::i64(), Type::i32(), Type::i32(), Type::ptr(), Type::ptr(), Type::i64()});
    build_method_call_stub(lowering, stub);
    build_prop_get_stub(lowering,
                        mod.create_function(kPropGetSlowStub, Type::i64(), {Type::i64(), Type::i32(), Type::ptr()}));
    build_prop_get_fast_stub(
        mod.create_function(kPropGetStub, Type::i64(), {Type::i64(), Type::i32(), Type::i64(), Type::i64()}));
    build_prop_get_fast_stub(
        mod.create_function(kPropGetFrontStub, Type::i64(), {Type::i64(), Type::i32(), Type::i64(), Type::i64()}),
        true);
    build_prop_set_stub(lowering, mod.create_function(kPropSetStub, Type::void_type(),
                                                      {Type::i64(), Type::i32(), Type::i64(), Type::ptr(), Type::i32()}));
    build_sort_merge_stub(
        mod.create_function(kSortMergeStub, Type::i64(), {Type::i64(), Type::i64(), Type::i32(), Type::ptr()}));
}

// A SLOT hit's entry (build_method_call_stub's SLOT and function-receiver
// arms, made at the site by il_method_site.cpp): `callee`, the value read out
// of the slot, re-tested at every call as the stub tests it (a function
// object of this realm that compiled code may enter) and entered with `recv`
// as `this`; anything else branches to `miss`. The builder is left after the
// call, in a block of its own.
Value* enter_slot_callee(Builder& b, Value* callee, Value* recv, uint32_t argc,
                         const std::function<Value*(Builder&)>& stage_argv, BasicBlock* miss) {
    BasicBlock* cur = b.current_block();
    BasicBlock* enter = b.append_block(block_prefix(b, "slotenter"));
    b.position_at_end(cur);
    const EntryTest t = entry_test_br(b, tls_block(b), callee, kCallMask, kCallBits, miss);
    b.build_br_if(t.ok, enter, miss);

    b.position_at_end(enter);
    Value* argv = stage_argv(b);
    Value* code = b.build_load(Type::ptr(), t.fn, BRONZE_ABI_FN_CODE_OFFSET);
    Value* env = b.build_load(Type::i64(), t.fn, BRONZE_ABI_FN_ENV_OFFSET);
    return call_code(b, code, env, recv, argc, argv);
}

// The direct entry's own test (entry_test: a function object, of this realm,
// callable directly) and the code word: a closure of `callee`. Everything a
// direct entry of it would skip, the inlined body skips the same way.
//
// As branches (entry_test_br): a value that is no function object answers
// no from a block of its own, so the function's header and words are read
// through its payload with no select in the address. The IL branches on the
// answer next (`closure.is` is always followed by its `br`), so the constant
// no is threaded straight to the branch's other side.
Value* lower_closure_is(IlLowering* lowering, Builder& b, Value* fn_val, const std::string& callee) {
    (void)lowering;
    const std::string prefix = block_prefix(b, "closis");
    BasicBlock* cur = b.current_block();
    BasicBlock* no = b.append_block(prefix + "_no");
    BasicBlock* join = b.append_block(prefix + "_join");
    Value* result = b.add_block_param(join, Type::i32());
    b.position_at_end(cur);
    const EntryTest t = entry_test_br(b, tls_block(b), fn_val, kCallMask, kCallBits, no);
    Value* code = b.build_load(Type::ptr(), t.fn, BRONZE_ABI_FN_CODE_OFFSET);
    b.build_br(join, {b.build_and(t.ok, b.build_eq(code, b.build_func_addr("__wrapper_" + callee)))});
    b.position_at_end(no);
    b.build_br(join, {b.build_iconst_i32(0)});
    b.position_at_end(join);
    return result;
}

Value* lower_closure_env(Builder& b, Value* fn_val) {
    Value* ptr = b.build_xor(fn_val, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    return b.build_load(Type::i64(), ptr, BRONZE_ABI_FN_ENV_OFFSET);
}

Value* lower_construct_direct(IlLowering* lowering, Builder& b, Value* ctor, const std::vector<Value*>& boxed_args) {
    const uint32_t argc = static_cast<uint32_t>(boxed_args.size());
    Value* argv = lowering->stage_argv(b, boxed_args);
    const std::string prefix = block_prefix(b, "fnew");
    BasicBlock* cur = b.current_block();
    BasicBlock* fast = b.append_block(prefix + "_fast");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    Value* tls = tls_block(b);
    const EntryTest t = entry_test_br(b, tls, ctor, kConstructMask, kConstructBits, slow);
    Value* shape = b.build_load(Type::i64(), t.fn, BRONZE_ABI_FN_INSTANCE_SHAPE_OFFSET);
    Value* ok = b.build_and(t.ok, b.build_ne(shape, b.build_iconst_i64(0)));
    Value* obj_bits = lowering->alloc_lowering().bump_plain_object(b, shape, ok, slow, fast);

    // The instance is held tagged across the call, which may collect, and
    // read back after it for the result rule.
    Value* obj = lowering->ensure_type(obj_bits, Type::tagged(), b);
    Value* code = b.build_load(Type::ptr(), t.fn, BRONZE_ABI_FN_CODE_OFFSET);
    Value* env = b.build_load(Type::i64(), t.fn, BRONZE_ABI_FN_ENV_OFFSET);
    Value* ret = call_code(b, code, env, obj_bits, argc, argv);
    Value* self = lowering->unbox_tagged(obj, b);
    Value* ret_is_obj = b.build_ult(b.build_xor(ret, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits))),
                                    b.build_iconst_i64(kPayloadLimit));
    b.build_br(merge, {b.build_select(ret_is_obj, ret, self)});

    b.position_at_end(slow);
    b.build_br(merge, {b.build_call("bronze_construct", Type::i64(),
                                    {ctor, b.build_iconst_i32(static_cast<int32_t>(argc)), argv})});

    b.position_at_end(merge);
    return result;
}

}  // namespace il2mir
