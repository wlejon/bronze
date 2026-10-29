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
    // The TLS block's first word is no header (bronze_abi.h, proto_epoch), so
    // the header compare alone refuses a non-object.
    Value* is_fn = b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kFunctionHeaderLow));
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

namespace {

// What a property site's cache says about a plain receiver, as the stubs
// below read it: whether one of the `ways` entries names the receiver's
// shape, that entry's slot word ((depth << 32) | slot, the flags in the
// depth half) and whether the depth > 0 answer it gives is still current
// (its epoch is the thread's).
struct PropEntry {
    Value* plain = nullptr;
    Value* ptr = nullptr;
    Value* shape = nullptr;
    Value* hit = nullptr;
    Value* slot_word = nullptr;
    Value* epoch_ok = nullptr;
};

PropEntry prop_entry(PropertyLoweringHelper& pl, Builder& b, Value* tls, Value* obj, Value* site, int ways) {
    PropEntry e;
    pl.receiver_shape(b, obj, e.plain, e.ptr, e.shape);
    Value* any = nullptr;
    Value* epoch = nullptr;
    // At most one way names a shape (an install rewrites an entry in place),
    // so the words select on the match alone.
    for (int32_t way = ways - 1; way >= 0; --way) {
        const int32_t at = way * static_cast<int32_t>(BRONZE_ABI_IC_ENTRY_SIZE);
        Value* match = b.build_eq(e.shape, b.build_load(Type::i64(), site, at + BRONZE_ABI_IC_SHAPE_OFFSET));
        Value* word = b.build_load(Type::i64(), site, at + BRONZE_ABI_IC_SLOT_OFFSET);
        Value* ep = b.build_load(Type::i64(), site, at + BRONZE_ABI_IC_EPOCH_OFFSET);
        e.slot_word = e.slot_word ? b.build_select(match, word, e.slot_word) : word;
        epoch = epoch ? b.build_select(match, ep, epoch) : ep;
        any = any ? b.build_or(any, match) : match;
    }
    e.hit = b.build_and(e.plain, any);
    e.epoch_ok = b.build_eq(epoch, b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF));
    return e;
}

constexpr int64_t kOwnDataLimit = int64_t{1} << 32;
constexpr int64_t kAccessorDepth = int64_t{BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG};
constexpr int64_t kSlotMask = 0xFFFFFFFFLL;
constexpr int32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;

// The deepest entry the stubs walk to; deeper ones are the helper's. Four
// links cover a three.js leaf class's accessors (Mesh -> Object3D ->
// EventDispatcher) with one to spare.
constexpr int64_t kMaxStubDepth = 4;

// Whether `depth` (the slot word's high half) is an entry at 1..kMaxStubDepth
// with no flag but, at most, the accessor's: not absent, not an f64 slot.
Value* walkable_depth(Builder& b, Value* depth) {
    Value* links = b.build_and(depth, b.build_iconst_i64(~kAccessorDepth));
    return b.build_ult(b.build_sub(links, b.build_iconst_i64(1)), b.build_iconst_i64(kMaxStubDepth));
}

// The holder of a depth-`depth` entry, walked the way
// ObjectHeader::cachedProtoHolder walks (shape -> root -> prototype, once a
// link): every link a plain object whose shape is no dictionary, else `slow`.
// The builder enters at the walk and leaves in the block where the result is
// the holder's payload. An entry whose receiver shape and epoch both still
// hold names the chain it was filled on, so what is left to ask at each link
// is what the fill asked of it.
Value* walk_to_prototype(Builder& b, Value* tls, Value* shape, Value* depth, BasicBlock* slow,
                         const std::string& prefix) {
    // append_block moves the insertion point.
    BasicBlock* entry = b.current_block();
    BasicBlock* walk = b.append_block(prefix + "_walk");
    Value* cur_shape = b.add_block_param(walk, Type::i64());
    Value* left = b.add_block_param(walk, Type::i64());
    BasicBlock* has_root = b.append_block(prefix + "_root");
    BasicBlock* is_plain = b.append_block(prefix + "_proto");
    BasicBlock* next = b.append_block(prefix + "_next");
    BasicBlock* found = b.append_block(prefix + "_holder");
    Value* holder = b.add_block_param(found, Type::i64());
    b.position_at_end(entry);
    b.build_br(walk, {shape, b.build_and(depth, b.build_iconst_i64(~kAccessorDepth))});

    b.position_at_end(walk);
    Value* root = b.build_load(Type::i64(), cur_shape, BRONZE_ABI_SHAPE_ROOT_OFFSET);
    b.build_br_if(b.build_ne(root, b.build_iconst_i64(0)), has_root, slow);

    b.position_at_end(has_root);
    Value* proto = b.build_load(Type::i64(), root, BRONZE_ABI_SHAPE_PROTO_OFFSET);
    Value* pptr = b.build_xor(proto, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    Value* is_obj = b.build_ult(pptr, b.build_iconst_i64(kPayloadLimit));
    Value* base = b.build_select(is_obj, pptr, tls);
    Value* plain = b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kPlainHeaderLow));
    b.build_br_if(plain, is_plain, slow);

    b.position_at_end(is_plain);
    Value* hshape = b.build_load(Type::i64(), pptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    Value* not_dict = b.build_eq(b.build_load(Type::i64(), hshape, BRONZE_ABI_SHAPE_DICT_OFFSET), b.build_iconst_i64(0));
    b.build_br_if(not_dict, next, slow);

    b.position_at_end(next);
    Value* rest = b.build_sub(left, b.build_iconst_i64(1));
    b.build_br_if(b.build_eq(rest, b.build_iconst_i64(0)), found, {pptr}, walk, {hshape, rest});

    b.position_at_end(found);
    return holder;
}

// The read stub: kPropGetStub(obj, key, site), bronze_prop_get's operands.
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

    b.position_at_end(cur);
    Value* tls = tls_block(b);
    const PropEntry e = prop_entry(pl, b, tls, obj, site, BRONZE_ABI_IC_WAYS);
    Value* own_data = b.build_ult(e.slot_word, b.build_iconst_i64(kOwnDataLimit));
    b.build_br_if(b.build_and(e.hit, own_data), own, not_own);

    b.position_at_end(own);
    b.build_ret(pl.load_slot(b, e.ptr, e.slot_word));

    b.position_at_end(not_own);
    Value* depth = b.build_lshr(e.slot_word, b.build_iconst_i64(32));
    Value* slot = b.build_and(e.slot_word, b.build_iconst_i64(kSlotMask));
    Value* current = b.build_and(e.hit, e.epoch_ok);
    b.build_br_if(b.build_and(current, b.build_eq(depth, b.build_iconst_i64(BRONZE_ABI_IC_DEPTH_ABSENT_FLAG))),
                  absent, not_absent);

    b.position_at_end(absent);
    b.build_ret(b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag)));

    // A chain entry with no flag but the accessor's.
    b.position_at_end(not_absent);
    Value* d1 = walkable_depth(b, depth);
    b.build_br_if(b.build_and(current, d1), depth1, not_depth1);

    b.position_at_end(depth1);
    Value* pptr = walk_to_prototype(b, tls, e.shape, depth, slow, prefix);
    b.build_br_if(b.build_ne(b.build_and(depth, b.build_iconst_i64(kAccessorDepth)), b.build_iconst_i64(0)), getter,
                  {pptr}, proto_data, {});

    b.position_at_end(proto_data);
    b.build_ret(pl.load_slot(b, pptr, slot));

    b.position_at_end(not_depth1);
    b.build_br_if(b.build_and(e.hit, b.build_eq(depth, b.build_iconst_i64(kAccessorDepth))), getter, {e.ptr}, slow,
                  {});

    b.position_at_end(getter);
    Value* fn_val = pl.load_slot(b, holder, slot);
    const EntryTest t = entry_test(b, tls, fn_val, kCallMask, kCallBits);
    b.build_br_if(t.ok, enter, slow);

    b.position_at_end(enter);
    Value* code = b.build_load(Type::ptr(), t.fn, BRONZE_ABI_FN_CODE_OFFSET);
    Value* env = b.build_load(Type::i64(), t.fn, BRONZE_ABI_FN_ENV_OFFSET);
    b.build_ret(call_code(b, code, env, obj, 0u, b.build_iconst_i64(0)));

    b.position_at_end(slow);
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
//    helper makes through rtEnterJs.
// Everything else, the shape transitions included, is the helper.
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

    b.position_at_end(cur);
    Value* argv = b.build_alloca_tagged(1);
    Value* tls = tls_block(b);
    const PropEntry e = prop_entry(pl, b, tls, obj, site, 1);
    Value* own_data = b.build_ult(e.slot_word, b.build_iconst_i64(kOwnDataLimit));
    b.build_br_if(b.build_and(e.hit, own_data), own, not_own);

    b.position_at_end(own);
    Value* base = pl.slot_base(b, e.ptr, e.slot_word);
    Value* slot_addr = b.build_add(b.build_add(base, b.build_shl(e.slot_word, b.build_iconst_i64(3))),
                                   b.build_iconst_i64(kBronzeObjSlotsOffset));
    b.build_store(Type::i64(), slot_addr, 0, val);
    Value* is_ref = b.build_ugt(val, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
    b.build_br_if(is_ref, barrier, done);

    b.position_at_end(barrier);
    b.build_call("brass_gc_write_barrier", Type::void_type(), {slot_addr, val});
    b.build_br(done);

    b.position_at_end(done);
    b.build_ret_void();

    b.position_at_end(not_own);
    Value* depth = b.build_lshr(e.slot_word, b.build_iconst_i64(32));
    Value* slot = b.build_and(e.slot_word, b.build_iconst_i64(kSlotMask));
    b.build_br_if(b.build_and(b.build_and(e.hit, e.epoch_ok),
                              b.build_and(walkable_depth(b, depth),
                                          b.build_ne(b.build_and(depth, b.build_iconst_i64(kAccessorDepth)),
                                                     b.build_iconst_i64(0)))),
                  depth1, not_depth1);

    b.position_at_end(depth1);
    Value* pptr = walk_to_prototype(b, tls, e.shape, depth, slow, prefix);
    b.build_br(setter, {pptr});

    b.position_at_end(not_depth1);
    b.build_br_if(b.build_and(e.hit, b.build_eq(depth, b.build_iconst_i64(kAccessorDepth))), setter, {e.ptr}, slow,
                  {});

    // An accessor's setter is the slot after its getter.
    b.position_at_end(setter);
    Value* fn_val = pl.load_slot(b, holder, b.build_add(slot, b.build_iconst_i64(1)));
    const EntryTest t = entry_test(b, tls, fn_val, kCallMask, kCallBits);
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

}  // namespace

void emit_direct_stubs(IlLowering* lowering, Module& mod) {
    const bool here = direct_method_ic_here(lowering);
    lowering->prop_lowering().set_ic_stubs(here);
    if (!here || mod.get_function(kMethodCallStub)) return;
    // Built before any body: a function built while another is being built
    // shares the module's builder state with it.
    Function* stub = mod.create_function(kMethodCallStub, Type::i64(),
                                         {Type::i64(), Type::i32(), Type::i32(), Type::ptr(), Type::ptr()});
    build_method_call_stub(lowering, stub);
    build_prop_get_stub(lowering, mod.create_function(kPropGetStub, Type::i64(), {Type::i64(), Type::i32(), Type::ptr()}));
    build_prop_set_stub(lowering, mod.create_function(kPropSetStub, Type::void_type(),
                                                      {Type::i64(), Type::i32(), Type::i64(), Type::ptr(), Type::i32()}));
}

Value* lower_method_call_ic(IlLowering* lowering, Builder& b, Value* recv, Value* key_id,
                            const std::vector<Value*>& boxed_args, Value* site) {
    Value* argv = lowering->stage_argv(b, boxed_args);
    return b.build_call(kMethodCallStub, Type::i64(),
                        {recv, key_id, b.build_iconst_i32(static_cast<int32_t>(boxed_args.size())), argv, site});
}

// The stub's SLOT arm (build_method_call_stub) at the site, in front of the
// stub itself: the same words read and the same tests made, so a receiver it
// answers for is one the stub would have entered the same function for. What
// it saves is the stub's call and its walk past way 0's DIRECT form, for the
// receivers that carry their callee — three.js's Euler and Quaternion inside
// an Object3D, whose `_onChangeCallback` is an own closure on every one.
Value* lower_method_slot_inline(IlLowering* lowering, Builder& b, Value* recv, const std::vector<Value*>& boxed_args,
                                Value* site, const std::function<Value*(Builder&)>& rest) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    const uint32_t argc = static_cast<uint32_t>(boxed_args.size());
    const std::string prefix = block_prefix(b, "slotcall");
    BasicBlock* cur = b.current_block();
    BasicBlock* slot = b.append_block(prefix + "_slot");
    BasicBlock* enter = b.append_block(prefix + "_enter");
    BasicBlock* other = b.append_block(prefix + "_rest");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    Value* plain = nullptr;
    Value* ptr = nullptr;
    Value* shape = nullptr;
    pl.receiver_shape(b, recv, plain, ptr, shape);
    Value* arity0 = b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_ARITY_WORD * kWord);
    Value* way0 = b.build_and(plain, b.build_eq(shape, b.build_load(Type::i64(), site, 0)));
    b.build_br_if(b.build_and(way0, b.build_uge(arity0, b.build_iconst_i64(kSlotFormLimit))), slot, other);

    // Re-read and re-tested at every call: same-shape receivers may hold
    // different functions in the slot.
    b.position_at_end(slot);
    Value* tls = tls_block(b);
    Value* slot_word = b.build_sub(b.build_lshr(arity0, b.build_iconst_i64(BRONZE_ABI_METHOD_IC_SLOT_SHIFT)),
                                   b.build_iconst_i64(1));
    const EntryTest t = entry_test(b, tls, pl.load_slot(b, ptr, slot_word), kCallMask, kCallBits);
    b.build_br_if(t.ok, enter, other);

    b.position_at_end(enter);
    Value* argv = lowering->stage_argv(b, boxed_args);
    Value* code = b.build_load(Type::ptr(), t.fn, BRONZE_ABI_FN_CODE_OFFSET);
    Value* env = b.build_load(Type::i64(), t.fn, BRONZE_ABI_FN_ENV_OFFSET);
    b.build_br(merge, {call_code(b, code, env, recv, argc, argv)});

    b.position_at_end(other);
    b.build_br(merge, {rest(b)});

    b.position_at_end(merge);
    return result;
}

// The slot lower_method_slot_inline would call through, read without the
// call: way 0 a SLOT entry for this receiver's shape, else `undefined`. The
// shape compare comes first because only a receiver of the entry's shape is
// known to be a plain object with that slot.
Value* lower_method_slot_fn(IlLowering* lowering, Builder& b, Value* recv, Value* keyed, Value* site) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    const std::string prefix = block_prefix(b, "slotfn");
    BasicBlock* cur = b.current_block();
    BasicBlock* slot = b.append_block(prefix + "_slot");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    Value* arity0 = b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_ARITY_WORD * kWord);
    Value* way0 = b.build_and(b.build_eq(keyed, b.build_load(Type::i64(), site, 0)),
                              b.build_uge(arity0, b.build_iconst_i64(kSlotFormLimit)));
    b.build_br_if(way0, slot, {}, merge, {b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag))});

    b.position_at_end(slot);
    Value* ptr = b.build_xor(recv, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    Value* slot_word = b.build_sub(b.build_lshr(arity0, b.build_iconst_i64(BRONZE_ABI_METHOD_IC_SLOT_SHIFT)),
                                   b.build_iconst_i64(1));
    b.build_br(merge, {pl.load_slot(b, ptr, slot_word)});

    b.position_at_end(merge);
    return result;
}

// The direct entry's own test (entry_test: a function object, of this realm,
// callable directly) and the code word: a closure of `callee`. Everything a
// direct entry of it would skip, the inlined body skips the same way.
Value* lower_closure_is(IlLowering* lowering, Builder& b, Value* fn_val, const std::string& callee) {
    (void)lowering;
    Value* tls = tls_block(b);
    const EntryTest t = entry_test(b, tls, fn_val, kCallMask, kCallBits);
    Value* code = b.build_load(Type::ptr(), t.fn, BRONZE_ABI_FN_CODE_OFFSET);
    return b.build_and(t.ok, b.build_eq(code, b.build_func_addr("__wrapper_" + callee)));
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
