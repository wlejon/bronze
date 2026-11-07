// A method call's site (il_lowering_direct.h, lower_method_call_ic): way 0's
// DIRECT entry tested and entered inline, way 1's out of line where the site
// met more than one receiver shape, and the module's method stub
// (build_method_call_stub, il_lowering_direct.cpp) for everything else; the
// stub's SLOT arms made at the site, for a receiver's own slot
// (lower_method_slot_inline) and a function's statics box
// (lower_method_fn_recv); and the SLOT callee read without the call
// (lower_method_slot_fn).
#include "il_abi.h"
#include "il_lowering.h"
#include "il_lowering_direct.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace il2mir {

namespace {

constexpr int32_t kWord = 8;
constexpr int32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int32_t kFunctionHeaderLow = (BRONZE_ABI_OBJ_FLAGS_FUNCTION << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr int64_t kSlotFormLimit = int64_t{1} << BRONZE_ABI_METHOD_IC_SLOT_SHIFT;
constexpr const char* kMethodCallStub = "__bronze_method_call_ic";

std::string block_prefix(Builder& b, const char* what) {
    return std::string(what) + "_" + std::to_string(b.current_block()->parent()->next_block_id());
}

Value* call_code(Builder& b, Value* code, Value* env, Value* this_val, uint32_t argc, Value* argv) {
    return b.build_call_indirect(code, Type::i64(), {env, this_val, b.build_iconst_i32(static_cast<int32_t>(argc)), argv});
}

// The stub's DIRECT arm for way 0 (build_method_call_stub), at the site: the
// receiver a plain object of way 0's shape, a DIRECT latch (the arity word's
// direct bit, no slot form) and its epoch the thread's, then the latched code
// and env entered here. Five of six method stub calls on three_meshes were
// this arm, and the stub's call, frame and dispatch were most of its cost.
// Out of line, way 0 a SLOT entry of the receiver's shape is the stub's SLOT
// arm made here (three.js's PropertyMixer `_mixBufferRegion` and WebGLState's
// object of closures: ~2.9K stub calls a frame on three_skinned, at sites no
// hint marks). Everything else calls the stub, which tests way 0 again and
// goes on.
Value* lower_method_way0_direct(IlLowering* lowering, Builder& b, Value* recv, Value* key_id, uint32_t argc,
                                Value* argv, Value* site, Value* row, bool way1) {
    const std::string prefix = block_prefix(b, "mway0");
    BasicBlock* cur = b.current_block();
    BasicBlock* object = b.append_block(prefix + "_object");
    BasicBlock* enter = b.append_block(prefix + "_enter");
    Value* enter_code = b.add_block_param(enter, Type::ptr());
    Value* enter_env = b.add_block_param(enter, Type::i64());
    BasicBlock* slot_test = b.append_block(prefix + "_slot_cold");
    BasicBlock* slot = b.append_block(prefix + "_slotcall_cold");
    BasicBlock* miss = way1 ? b.append_block(prefix + "_way1_cold") : nullptr;
    BasicBlock* stub = b.append_block(prefix + "_stub_cold");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());
    stub->set_cold();
    slot_test->set_cold();
    slot->set_cold();
    if (miss) miss->set_cold();
    // Tests by shifts against immediates, not masks: brass keeps a 64-bit
    // constant live from its first definition, and a large caller then held
    // each mask in a register or a stack slot across its whole body.
    auto imm = [&](int64_t v) { return b.build_iconst_i64(v); };

    b.position_at_end(cur);
    b.build_br_if(b.build_eq(b.build_lshr(recv, imm(48)), imm(BRONZE_ABI_TAG_OBJECT)), object, stub);
    BasicBlock* way0_miss = miss ? miss : stub;

    // A plain object of way 0's shape; the arity word's bit 31 (the direct
    // bit) its highest set bit (no SLOT form above it); the latch's epoch.
    // Way 0's words from the site's method row, the env from the site.
    b.position_at_end(object);
    const MethodWay0 w = method_way0(site, row);
    Value* ptr = b.build_lshr(b.build_shl(recv, imm(16)), imm(16));
    static_assert(BRONZE_ABI_METHOD_IC_DIRECT_ENTRY_BIT == (uint64_t{1} << 31) &&
                      BRONZE_ABI_METHOD_IC_SLOT_SHIFT == 32,
                  "the DIRECT form's test is one shift");
    Value* arity0 = b.build_load(Type::i64(), w.base, w.arity);
    Value* epoch = b.build_load(Type::i64(), b.build_pinned_tls_read(), BRONZE_TLS_PROTO_EPOCH_OFF);
    Value* same = b.build_and(
        b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kPlainHeaderLow)),
        b.build_eq(b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET), b.build_load(Type::i64(), w.base, w.shape)));
    Value* ok = b.build_and(same, b.build_and(b.build_eq(b.build_lshr(arity0, imm(31)), imm(1)),
                                              b.build_eq(b.build_load(Type::i64(), w.base, w.epoch), epoch)));
    b.build_br_if(ok, enter,
                  {b.build_load(Type::ptr(), w.base, w.code),
                   b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_ENV_WORD * kWord)},
                  slot_test, {});

    // Way 0 a SLOT entry of this receiver's shape (the stub's SLOT arm, out
    // of line): the callee re-read from the receiver's own slot and entered
    // as the stub would enter it.
    b.position_at_end(slot_test);
    b.build_br_if(b.build_and(same, b.build_uge(arity0, imm(kSlotFormLimit))), slot, way0_miss);
    b.position_at_end(slot);
    {
        Value* slot_word = b.build_sub(b.build_lshr(arity0, imm(BRONZE_ABI_METHOD_IC_SLOT_SHIFT)), imm(1));
        Value* entered = enter_slot_callee(b, lowering->prop_lowering().load_slot(b, ptr, slot_word), recv, argc,
                                           [&](Builder&) { return argv; }, way0_miss);
        b.build_br(merge, {entered});
    }

    // Way 1, the entry the latch displaced (a DIRECT entry and nothing else,
    // bronze_abi.h "WAY 1"): the receiver a plain object of its shape, the
    // direct bit and no slot form, its own epoch the thread's. Only an
    // object reaches here; the stub tests everything again.
    if (miss) {
        b.position_at_end(miss);
        Value* p1 = b.build_lshr(b.build_shl(recv, imm(16)), imm(16));
        Value* arity1 = b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_WAY1_ARITY_WORD * kWord);
        Value* epoch1 = b.build_load(Type::i64(), b.build_pinned_tls_read(), BRONZE_TLS_PROTO_EPOCH_OFF);
        Value* hit1 = b.build_and(
            b.build_and(b.build_eq(b.build_load(Type::i32(), p1, 0), b.build_iconst_i32(kPlainHeaderLow)),
                        b.build_eq(b.build_load(Type::i64(), p1, BRONZE_ABI_OBJ_SHAPE_OFFSET),
                                   b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_WAY1_SHAPE_WORD * kWord))),
            b.build_and(b.build_eq(b.build_lshr(arity1, imm(31)), imm(1)),
                        b.build_eq(b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_WAY1_EPOCH_WORD * kWord),
                                   epoch1)));
        b.build_br_if(hit1, enter,
                      {b.build_load(Type::ptr(), site, BRONZE_ABI_METHOD_IC_WAY1_CODE_WORD * kWord),
                       b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD * kWord)},
                      stub, {});
    }

    b.position_at_end(enter);
    b.build_br(merge, {call_code(b, enter_code, enter_env, recv, argc, argv)});

    b.position_at_end(stub);
    b.build_br(merge, {b.build_call(kMethodCallStub, Type::i64(),
                                    {recv, key_id, b.build_iconst_i32(static_cast<int32_t>(argc)), argv, site,
                                     row ? row : b.build_iconst_i64(0)})});

    b.position_at_end(merge);
    return result;
}

}  // namespace

MethodWay0 method_way0(Value* site, Value* row) {
    static_assert(BRONZE_ABI_METHOD_ROW_CODE_OFFSET == BRONZE_ABI_METHOD_IC_CODE_WORD * kWord &&
                      BRONZE_ABI_METHOD_ROW_ARITY_OFFSET == BRONZE_ABI_METHOD_IC_ARITY_WORD * kWord,
                  "a row's first three words are way 0's");
    if (row) {
        return {row, BRONZE_ABI_METHOD_ROW_SHAPE_OFFSET, BRONZE_ABI_METHOD_ROW_CODE_OFFSET,
                BRONZE_ABI_METHOD_ROW_ARITY_OFFSET, BRONZE_ABI_METHOD_ROW_EPOCH_OFFSET};
    }
    return {site, 0, BRONZE_ABI_METHOD_IC_CODE_WORD * kWord, BRONZE_ABI_METHOD_IC_ARITY_WORD * kWord,
            BRONZE_ABI_METHOD_IC_EPOCH_WORD * kWord};
}

// Move to front for method sites, as kPropGetFrontStub is for reads. Way 0
// is the latest latch and way 1 the entry it displaced, so a site meeting
// two shapes kept whichever latched last in way 0 however rarely it came:
// three_skinned's `children[i].updateMatrixWorld(force)` held a mesh there in
// some runs and sent every Bone to the stub (~7K calls a frame). A way-1 hit
// swaps the two DIRECT entries, and the site's inline test then names the
// shape that came last. Only a plain DIRECT resident moves into way 1
// (displaceMethodWay0 asks the same of a latch): word 0 a shape, no SLOT
// form, a code word. The env words are root cells the runtime remembers only
// when it writes a young value into one (rtNoteMethodIcEnvWords), so the
// swap is left out while either holds a young object, as moving it to the
// other word would put it in a cell no remembered set names.
void build_method_way1_front(Builder& b, Value* site, Value* row, Value* tls, Value* code1, Value* env1,
                             BasicBlock* enter) {
    const std::string prefix = block_prefix(b, "mfront");
    BasicBlock* cur = b.current_block();
    BasicBlock* swap = b.append_block(prefix + "_swap");
    auto imm = [&](int64_t v) { return b.build_iconst_i64(v); };
    auto word = [&](int32_t w) { return b.build_load(Type::i64(), site, w * kWord); };
    b.position_at_end(cur);
    Value* young_base = b.build_load(Type::i64(), tls, BRONZE_TLS_YOUNG_BASE_OFF);
    Value* young_span = b.build_load(Type::i64(), tls, BRONZE_TLS_YOUNG_SPAN_OFF);
    auto old_or_none = [&](Value* v) {
        Value* address = b.build_lshr(b.build_shl(v, imm(16)), imm(16));
        return b.build_uge(b.build_sub(address, young_base), young_span);
    };
    Value* w0 = word(0);
    Value* env0 = word(BRONZE_ABI_METHOD_IC_ENV_WORD);
    Value* direct0 = b.build_and(
        b.build_and(b.build_ne(w0, imm(0)),
                    b.build_eq(b.build_and(w0, imm(static_cast<int64_t>(BRONZE_ABI_METHOD_IC_EXOTIC_BIT))), imm(0))),
        b.build_and(b.build_eq(b.build_lshr(word(BRONZE_ABI_METHOD_IC_ARITY_WORD), imm(BRONZE_ABI_METHOD_IC_SLOT_SHIFT)),
                               imm(0)),
                    b.build_ne(word(BRONZE_ABI_METHOD_IC_CODE_WORD), imm(0))));
    b.build_br_if(b.build_and(direct0, b.build_and(old_or_none(env0), old_or_none(env1))), swap, {}, enter, {code1, env1});

    b.position_at_end(swap);
    static_assert(BRONZE_ABI_METHOD_IC_WAY1_SHAPE_WORD == 6 && BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD == 9 &&
                      BRONZE_ABI_METHOD_IC_WAY1_EPOCH_WORD == 10 && BRONZE_ABI_METHOD_IC_EPOCH_WORD == 5,
                  "way 1 is way 0's words 0-3 at 6-9, its epoch word 5 at 10");
    const int32_t pairs[][2] = {{0, BRONZE_ABI_METHOD_IC_WAY1_SHAPE_WORD},
                                {BRONZE_ABI_METHOD_IC_CODE_WORD, BRONZE_ABI_METHOD_IC_WAY1_CODE_WORD},
                                {BRONZE_ABI_METHOD_IC_ARITY_WORD, BRONZE_ABI_METHOD_IC_WAY1_ARITY_WORD},
                                {BRONZE_ABI_METHOD_IC_ENV_WORD, BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD},
                                {BRONZE_ABI_METHOD_IC_EPOCH_WORD, BRONZE_ABI_METHOD_IC_WAY1_EPOCH_WORD}};
    std::vector<std::pair<Value*, Value*>> loaded;
    for (const auto& p : pairs) loaded.emplace_back(word(p[0]), word(p[1]));
    for (size_t i = 0; i < loaded.size(); ++i) {
        b.build_store(Type::i64(), site, pairs[i][0] * kWord, loaded[i].second);
        b.build_store(Type::i64(), site, pairs[i][1] * kWord, loaded[i].first);
    }
    // The method row (bronze_abi_layout.h, "the METHOD ROWS") takes way 0's
    // new guard words with it; a site without one passes a null row.
    BasicBlock* row_bb = b.append_block(prefix + "_row");
    b.position_at_end(swap);
    b.build_br_if(b.build_ne(row, imm(0)), row_bb, {}, enter, {code1, env1});
    b.position_at_end(row_bb);
    b.build_store(Type::i64(), row, BRONZE_ABI_METHOD_ROW_SHAPE_OFFSET, loaded[0].second);
    b.build_store(Type::i64(), row, BRONZE_ABI_METHOD_ROW_CODE_OFFSET, loaded[1].second);
    b.build_store(Type::i64(), row, BRONZE_ABI_METHOD_ROW_ARITY_OFFSET, loaded[2].second);
    b.build_store(Type::i64(), row, BRONZE_ABI_METHOD_ROW_EPOCH_OFFSET, loaded[4].second);
    b.build_br(enter, {code1, env1});
}

// The slot lower_method_slot_inline would call through, read without the
// call: way 0 a SLOT entry for this receiver's shape, else `undefined`. The
// shape compare comes first because only a receiver of the entry's shape is
// known to be a plain object with that slot.
Value* lower_method_slot_fn(IlLowering* lowering, Builder& b, Value* recv, Value* keyed, Value* site, Value* row) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    const std::string prefix = block_prefix(b, "slotfn");
    BasicBlock* cur = b.current_block();
    BasicBlock* slot = b.append_block(prefix + "_slot");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    const MethodWay0 w = method_way0(site, row);
    Value* arity0 = b.build_load(Type::i64(), w.base, w.arity);
    Value* way0 = b.build_and(b.build_eq(keyed, b.build_load(Type::i64(), w.base, w.shape)),
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

// The stub's SLOT arm (build_method_call_stub) at the site, in front of the
// stub itself: the same words read and the same tests made, so a receiver it
// answers for is one the stub would have entered the same function for. What
// it saves is the stub's call and its walk past way 0's DIRECT form, for the
// receivers that carry their callee — three.js's Euler and Quaternion inside
// an Object3D, whose `_onChangeCallback` is an own closure on every one.
Value* lower_method_slot_inline(IlLowering* lowering, Builder& b, Value* recv, const std::vector<Value*>& boxed_args,
                                Value* site, Value* row, const std::function<Value*(Builder&)>& rest) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    const std::string prefix = block_prefix(b, "slotcall");
    BasicBlock* cur = b.current_block();
    BasicBlock* slot = b.append_block(prefix + "_slot");
    BasicBlock* other = b.append_block(prefix + "_rest");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    Value* ptr = b.build_xor(recv, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    const MethodWay0 w = method_way0(site, row);
    Value* arity0 = b.build_load(Type::i64(), w.base, w.arity);
    Value* way0 = b.build_eq(pl.keyed_shape(b, recv), b.build_load(Type::i64(), w.base, w.shape));
    b.build_br_if(b.build_and(way0, b.build_uge(arity0, b.build_iconst_i64(kSlotFormLimit))), slot, other);

    // Re-read and re-tested at every call: same-shape receivers may hold
    // different functions in the slot.
    b.position_at_end(slot);
    Value* slot_word = b.build_sub(b.build_lshr(arity0, b.build_iconst_i64(BRONZE_ABI_METHOD_IC_SLOT_SHIFT)),
                                   b.build_iconst_i64(1));
    Value* entered = enter_slot_callee(b, pl.load_slot(b, ptr, slot_word), recv, static_cast<uint32_t>(boxed_args.size()),
                                       [&](Builder& bb) { return lowering->stage_argv(bb, boxed_args); }, other);
    b.build_br(merge, {entered});

    b.position_at_end(other);
    b.build_br(merge, {rest(b)});

    b.position_at_end(merge);
    return result;
}

// The stub's FUNCTION-receiver arm (build_method_call_stub, `mcall_fnrecv`)
// at the site, ahead of everything else, for a receiver that names a function
// binding: way 0 the SLOT form keyed on the shape of the receiver's statics
// box (latchFunctionStaticsMethodIc), a plain box of that shape holding the
// key at that slot, and the callee read there entered as a SLOT hit enters
// one. The same words and tests as the stub's arm, which a function receiver
// reached only after way 0, way 1 and the EXOTIC forms had each missed:
// `Quaternion.slerpFlat(...)` went through all of it ~5K times a frame on
// three_skinned. A function's keyed shape names no way, so no arm the stub
// tests first could have answered such a receiver differently.
Value* lower_method_fn_recv(IlLowering* lowering, Builder& b, Value* recv, const std::vector<Value*>& boxed_args,
                            Value* site, Value* row, const std::function<Value*(Builder&)>& rest) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    const std::string prefix = block_prefix(b, "fnrecvcall");
    BasicBlock* cur = b.current_block();
    BasicBlock* fobj = b.append_block(prefix + "_obj");
    BasicBlock* fbox = b.append_block(prefix + "_box");
    BasicBlock* fshape = b.append_block(prefix + "_shape");
    BasicBlock* fslot = b.append_block(prefix + "_slot");
    BasicBlock* other = b.append_block(prefix + "_rest");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    // Branches, not a select chain: each word is read only once the one
    // before it says it may be.
    b.position_at_end(cur);
    Value* ptr = b.build_xor(recv, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(ptr, b.build_iconst_i64(kPayloadLimit)), fobj, other);
    b.position_at_end(fobj);
    const MethodWay0 w = method_way0(site, row);
    Value* arity0 = b.build_load(Type::i64(), w.base, w.arity);
    b.build_br_if(b.build_and(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kFunctionHeaderLow)),
                              b.build_uge(arity0, b.build_iconst_i64(kSlotFormLimit))),
                  fbox, other);
    b.position_at_end(fbox);
    Value* box_ptr = b.build_xor(b.build_load(Type::i64(), ptr, BRONZE_ABI_FN_PROPERTIES_OFFSET),
                                 b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(box_ptr, b.build_iconst_i64(kPayloadLimit)), fshape, other);
    b.position_at_end(fshape);
    Value* box_ok = b.build_and(
        b.build_eq(b.build_load(Type::i32(), box_ptr, 0), b.build_iconst_i32(kPlainHeaderLow)),
        b.build_eq(b.build_load(Type::i64(), box_ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET), b.build_load(Type::i64(), w.base, w.shape)));
    b.build_br_if(box_ok, fslot, other);

    b.position_at_end(fslot);
    Value* slot_word = b.build_sub(b.build_lshr(arity0, b.build_iconst_i64(BRONZE_ABI_METHOD_IC_SLOT_SHIFT)),
                                   b.build_iconst_i64(1));
    Value* callee = pl.load_slot_br(b, box_ptr, slot_word);
    Value* entered = enter_slot_callee(b, callee, recv, static_cast<uint32_t>(boxed_args.size()),
                                       [&](Builder& bb) { return lowering->stage_argv(bb, boxed_args); }, other);
    b.build_br(merge, {entered});

    b.position_at_end(other);
    b.build_br(merge, {rest(b)});

    b.position_at_end(merge);
    return result;
}

Value* lower_method_call_ic(IlLowering* lowering, Builder& b, Value* recv, Value* key_id,
                            const std::vector<Value*>& boxed_args, Value* site, Value* row, bool way1) {
    Value* argv = lowering->stage_argv(b, boxed_args);
    const uint32_t argc = static_cast<uint32_t>(boxed_args.size());
    // The inline arm calls through a register, which a block with a handler
    // cannot invoke, and is only size in code that runs once.
    if (direct_entry_here(lowering)) {
        return lower_method_way0_direct(lowering, b, recv, key_id, argc, argv, site, row, way1);
    }
    return b.build_call(kMethodCallStub, Type::i64(),
                        {recv, key_id, b.build_iconst_i32(static_cast<int32_t>(argc)), argv, site,
                         row ? row : b.build_iconst_i64(0)});
}

}  // namespace il2mir
