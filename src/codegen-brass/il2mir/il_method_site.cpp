// A method call's site (il_lowering_direct.h, lower_method_call_ic): way 0's
// DIRECT entry tested and entered inline, way 1's out of line where the site
// met more than one receiver shape, and the module's method stub
// (build_method_call_stub, il_lowering_direct.cpp) for everything else.
#include "il_abi.h"
#include "il_lowering.h"
#include "il_lowering_direct.h"

#include <string>

namespace il2mir {

namespace {

constexpr int32_t kWord = 8;
constexpr int32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
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
// Everything else calls the stub, which tests way 0 again and goes on.
Value* lower_method_way0_direct(Builder& b, Value* recv, Value* key_id, uint32_t argc, Value* argv, Value* site,
                                bool way1) {
    const std::string prefix = block_prefix(b, "mway0");
    BasicBlock* cur = b.current_block();
    BasicBlock* object = b.append_block(prefix + "_object");
    BasicBlock* enter = b.append_block(prefix + "_enter");
    Value* enter_code = b.add_block_param(enter, Type::ptr());
    Value* enter_env = b.add_block_param(enter, Type::i64());
    BasicBlock* miss = way1 ? b.append_block(prefix + "_way1_cold") : nullptr;
    BasicBlock* stub = b.append_block(prefix + "_stub_cold");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());
    stub->set_cold();
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
    b.position_at_end(object);
    Value* ptr = b.build_lshr(b.build_shl(recv, imm(16)), imm(16));
    static_assert(BRONZE_ABI_METHOD_IC_DIRECT_ENTRY_BIT == (uint64_t{1} << 31) &&
                      BRONZE_ABI_METHOD_IC_SLOT_SHIFT == 32,
                  "the DIRECT form's test is one shift");
    Value* arity0 = b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_ARITY_WORD * kWord);
    Value* epoch = b.build_load(Type::i64(), b.build_pinned_tls_read(), BRONZE_TLS_PROTO_EPOCH_OFF);
    Value* ok = b.build_and(
        b.build_and(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kPlainHeaderLow)),
                    b.build_eq(b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET), b.build_load(Type::i64(), site, 0))),
        b.build_and(b.build_eq(b.build_lshr(arity0, imm(31)), imm(1)),
                    b.build_eq(b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_EPOCH_WORD * kWord), epoch)));
    b.build_br_if(ok, enter,
                  {b.build_load(Type::ptr(), site, BRONZE_ABI_METHOD_IC_CODE_WORD * kWord),
                   b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_ENV_WORD * kWord)},
                  way0_miss, {});

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
                                    {recv, key_id, b.build_iconst_i32(static_cast<int32_t>(argc)), argv, site})});

    b.position_at_end(merge);
    return result;
}

}  // namespace

Value* lower_method_call_ic(IlLowering* lowering, Builder& b, Value* recv, Value* key_id,
                            const std::vector<Value*>& boxed_args, Value* site, bool way1) {
    Value* argv = lowering->stage_argv(b, boxed_args);
    const uint32_t argc = static_cast<uint32_t>(boxed_args.size());
    // The inline arm calls through a register, which a block with a handler
    // cannot invoke, and is only size in code that runs once.
    if (direct_entry_here(lowering)) return lower_method_way0_direct(b, recv, key_id, argc, argv, site, way1);
    return b.build_call(kMethodCallStub, Type::i64(),
                        {recv, key_id, b.build_iconst_i32(static_cast<int32_t>(argc)), argv, site});
}

}  // namespace il2mir
