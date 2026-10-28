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

#include "il_abi.h"
#include "il_lowering.h"

#include <string>

namespace il2mir {

namespace {

constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr int32_t kFunctionHeaderLow = (BRONZE_ABI_OBJ_FLAGS_FUNCTION << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int32_t kWord = 8;
constexpr int64_t kSlotFormLimit = int64_t{1} << BRONZE_ABI_METHOD_IC_SLOT_SHIFT;
// The flags word's realm bytes, as a mask over the whole word.
constexpr uint64_t kRealmMask = ~uint64_t{0} << BRONZE_ABI_FN_REALM_SHIFT;
// A call's test: the direct-call bit set and the class-constructor bit clear
// (a call of a class constructor throws, which only the helper does).
constexpr uint64_t kCallMask = kRealmMask | BRONZE_ABI_FN_FLAG_DIRECT_CALL | BRONZE_ABI_FN_FLAG_CLASS_CTOR;
constexpr uint64_t kCallBits = BRONZE_ABI_FN_FLAG_DIRECT_CALL;
// A construction's: the direct-call bit set and the vet byte exactly 1 — the
// helper's plain path has run for this function object, so it is no bound
// function, no primitive-wrapper intrinsic and no native base, and its
// prototype and instance shape exist (bronze_abi.h, the inline `new`
// contract). A class constructor may be constructed this way.
constexpr int kVetShift = (BRONZE_ABI_FN_CTOR_VETTED_OFFSET - BRONZE_ABI_FN_FLAGS_WORD_OFFSET) * 8;
constexpr uint64_t kConstructMask = kRealmMask | (uint64_t{0xFF} << kVetShift) | BRONZE_ABI_FN_FLAG_DIRECT_CALL;
constexpr uint64_t kConstructBits = (uint64_t{1} << kVetShift) | BRONZE_ABI_FN_FLAG_DIRECT_CALL;
static_assert(BRONZE_ABI_FN_REALM_SHIFT > kVetShift + 8, "the realm bytes sit above the vet byte");

Value* tls_block(Builder& b) {
    Module* mod = b.current_block()->parent()->parent();
    return (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                               : b.build_call("bronze_tls_block_addr", Type::i64(), {});
}

std::string block_prefix(Builder& b, const char* what) {
    return std::string(what) + "_" + std::to_string(b.current_block()->parent()->next_block_id());
}

// Whether `callee` is a function object whose flags word, under `mask`, is
// `bits` with this thread's current realm in the realm bytes; and the
// address its fields are read through, which is the TLS block (mapped, and
// never read for an answer) for anything else, so no load here can fault.
struct EntryTest {
    Value* ok = nullptr;
    Value* fn = nullptr;
};

EntryTest entry_test(Builder& b, Value* tls, Value* callee, uint64_t mask, uint64_t bits) {
    Value* ptr = b.build_xor(callee, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    Value* is_obj = b.build_ult(ptr, b.build_iconst_i64(kPayloadLimit));
    Value* base = b.build_select(is_obj, ptr, tls);
    Value* is_fn = b.build_and(is_obj, b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kFunctionHeaderLow)));
    // A non-function's payload may be shorter than a function's, so its words
    // past the header are not read at all.
    Value* fn = b.build_select(is_fn, ptr, tls);
    Value* word = b.build_load(Type::i64(), fn, BRONZE_ABI_FN_FLAGS_WORD_OFFSET);
    Value* realm = b.build_shl(b.build_load(Type::i64(), tls, BRONZE_TLS_REALM_SLOT_OFF),
                               b.build_iconst_i64(BRONZE_ABI_FN_REALM_SHIFT));
    Value* want = b.build_or(realm, b.build_iconst_i64(static_cast<int64_t>(bits)));
    Value* flags_ok = b.build_eq(b.build_and(word, b.build_iconst_i64(static_cast<int64_t>(mask))), want);
    return EntryTest{b.build_and(is_fn, flags_ok), fn};
}

Value* call_code(Builder& b, Value* code, Value* env, Value* this_val, Value* argc, Value* argv) {
    return b.build_call_indirect(code, Type::i64(), {env, this_val, argc, argv});
}

Value* call_code(Builder& b, Value* code, Value* env, Value* this_val, uint32_t argc, Value* argv) {
    return call_code(b, code, env, this_val, b.build_iconst_i32(static_cast<int32_t>(argc)), argv);
}

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
    Value* argv = lowering->stage_argv(b, boxed_args);
    const std::string prefix = block_prefix(b, "fcall");
    BasicBlock* cur = b.current_block();
    BasicBlock* enter = b.append_block(prefix + "_enter");
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    Value* tls = tls_block(b);
    const EntryTest t = entry_test(b, tls, callee, kCallMask, kCallBits);
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
    const std::string prefix = "mcall";
    BasicBlock* way0 = b.append_block(prefix + "_way0");
    BasicBlock* direct0 = b.append_block(prefix + "_direct0");
    BasicBlock* slot = b.append_block(prefix + "_slot");
    BasicBlock* not_way0 = b.append_block(prefix + "_notway0");
    BasicBlock* exotic = b.append_block(prefix + "_exotic");
    BasicBlock* exotic_box = b.append_block(prefix + "_exotic_box");
    BasicBlock* enter = b.append_block(prefix + "_enter");
    Value* enter_code = b.add_block_param(enter, Type::ptr());
    Value* enter_env = b.add_block_param(enter, Type::i64());
    BasicBlock* slow = b.append_block(prefix + "_slow");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    Value* tls = tls_block(b);
    Value* plain = nullptr;
    Value* ptr = nullptr;
    Value* shape = nullptr;
    pl.receiver_shape(b, recv, plain, ptr, shape);
    Value* w0 = b.build_load(Type::i64(), site, 0);
    Value* arity0 = b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_ARITY_WORD * kWord);
    Value* direct_bit = b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_METHOD_IC_DIRECT_ENTRY_BIT));
    Value* zero = b.build_iconst_i64(0);
    b.build_br_if(b.build_and(plain, b.build_eq(shape, w0)), way0, not_way0);

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
        Value* callee = pl.load_slot(b, ptr, slot_word);
        const EntryTest t = entry_test(b, tls, callee, kCallMask, kCallBits);
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
            b.build_and(plain, b.build_eq(shape, b.build_load(Type::i64(), site, w1 * kWord))),
            b.build_and(
                b.build_and(b.build_ne(b.build_and(arity1, direct_bit), zero),
                            b.build_ult(arity1, b.build_iconst_i64(kSlotFormLimit))),
                b.build_eq(b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_WAY1_EPOCH_WORD * kWord), epoch)));
        Value* code = b.build_load(Type::ptr(), site, BRONZE_ABI_METHOD_IC_WAY1_CODE_WORD * kWord);
        Value* env = b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD * kWord);
        b.build_br_if(hit, enter, {code, env}, exotic, {});
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
        Value* is_obj = b.build_ult(ptr, b.build_iconst_i64(kPayloadLimit));
        Value* base = b.build_select(is_obj, ptr, tls);
        Value* kind = b.build_lshr(b.build_and(w0, b.build_iconst_i64(0xFFFFFFFFLL)),
                                   b.build_iconst_i64(BRONZE_ABI_METHOD_IC_KIND_SHIFT));
        Value* want_low = b.build_or(b.build_shl(kind, b.build_iconst_i64(16)), b.build_iconst_i64(BRONZE_ABI_TAG_OBJECT));
        Value* kind_ok = b.build_eq(b.build_zext_i64(b.build_load(Type::i32(), base, 0)), want_low);
        Value* entry_ok = b.build_and(b.build_ne(b.build_and(arity0, direct_bit), zero),
                                      b.build_ult(arity0, b.build_iconst_i64(kSlotFormLimit)));
        b.build_br_if(b.build_and(b.build_and(box_form, is_obj), b.build_and(kind_ok, entry_ok)), exotic_box, slow);
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

    b.position_at_end(enter);
    b.build_br(merge, {call_code(b, enter_code, enter_env, recv, argc, argv)});

    b.position_at_end(slow);
    b.build_br(merge, {b.build_call("bronze_call_method", Type::i64(),
                                    {recv, key_id, argc, argv, site})});

    b.position_at_end(merge);
    b.build_ret(result);
    fn->rebuild_cfg_predecessors();
}

void emit_direct_stubs(IlLowering* lowering, Module& mod) {
    if (!direct_method_ic_here(lowering) || mod.get_function(kMethodCallStub)) return;
    // Built before any body: a function built while another is being built
    // shares the module's builder state with it.
    Function* stub = mod.create_function(kMethodCallStub, Type::i64(),
                                         {Type::i64(), Type::i32(), Type::i32(), Type::ptr(), Type::ptr()});
    build_method_call_stub(lowering, stub);
}

Value* lower_method_call_ic(IlLowering* lowering, Builder& b, Value* recv, Value* key_id,
                            const std::vector<Value*>& boxed_args, Value* site) {
    Value* argv = lowering->stage_argv(b, boxed_args);
    return b.build_call(kMethodCallStub, Type::i64(),
                        {recv, key_id, b.build_iconst_i32(static_cast<int32_t>(boxed_args.size())), argv, site});
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
    const EntryTest t = entry_test(b, tls, ctor, kConstructMask, kConstructBits);
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
