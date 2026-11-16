// Calls as a baseline JIT makes them: the callee tested inline and entered
// without the runtime when it may be, the runtime's helper behind each test.
//
// What lets compiled code enter a function's code itself is one word of the
// function object (bronze_abi.h, BRONZE_ABI_FN_FLAG_DIRECT_CALL, the flags
// word at BRONZE_ABI_FN_FLAGS_WORD_OFFSET): the direct-call bit, no class
// constructor bit for a call, and the function's realm the thread's. Every
// call shape here ends in the same entry: `code(env, this, argc, argv)` with
// the arguments in the function's argv block, which roots them. A short call
// needs no padding: a compiled wrapper reads an absent argument as undefined.
//
// Before this, every call went through bronze_call_dynamic_N /
// bronze_call_method / bronze_construct_N, and the runtime's entry (realm
// scope, new.target scope, the generated-code entry scope) was most of a
// small function's call.
#include "il_call_entry.h"

#include "il_abi.h"
#include "il_lowering.h"
#include "il_prop_stubs.h"

#include <brass/mir/module.hpp>

namespace il2mir {

namespace {

constexpr int32_t kWord = 8;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr int32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int32_t kFunctionHeaderLow = (BRONZE_ABI_OBJ_FLAGS_FUNCTION << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr uint64_t kRealmMask = ~uint64_t{0} << BRONZE_ABI_FN_REALM_SHIFT;
// A call: direct-call bit set, class-constructor bit clear, and either a
// compiled function of the caller's realm or a native (realm 0, every
// realm's) whose arity the arguments cover — C++ reads argv up to its arity,
// where a compiled function's wrapper pads (bronze_abi.h).
constexpr uint64_t kCallMask =
    kRealmMask | BRONZE_ABI_FN_FLAG_DIRECT_CALL | BRONZE_ABI_FN_FLAG_CLASS_CTOR | BRONZE_ABI_FN_FLAG_NATIVE;
constexpr uint64_t kCallBits = BRONZE_ABI_FN_FLAG_DIRECT_CALL;
constexpr uint64_t kNativeCallBits = BRONZE_ABI_FN_FLAG_DIRECT_CALL | BRONZE_ABI_FN_FLAG_NATIVE;
// A `new`: a compiled function with the direct-call bit and the
// construct-vetted byte (set only once bronze_construct took its plain path
// for the function), the realm. A class constructor may be entered by `new`;
// a native never here (its `new` makes its own object).
constexpr int kVetShift = (BRONZE_ABI_FN_CTOR_VETTED_OFFSET - BRONZE_ABI_FN_FLAGS_WORD_OFFSET) * 8;
constexpr uint64_t kConstructMask = kRealmMask | (uint64_t{0xFF} << kVetShift) | BRONZE_ABI_FN_FLAG_DIRECT_CALL |
                                    BRONZE_ABI_FN_FLAG_NATIVE;
constexpr uint64_t kConstructBits = (uint64_t{1} << kVetShift) | BRONZE_ABI_FN_FLAG_DIRECT_CALL;

static_assert(BRONZE_ABI_METHOD_IC_DIRECT_ENTRY_BIT == (uint64_t{1} << 31) && BRONZE_ABI_METHOD_IC_SLOT_SHIFT == 32,
              "a DIRECT arity word is the arity with bit 31 set and a zero high half");

Value* imm(Builder& b, int64_t v) { return b.build_iconst_i64(v); }

std::string prefix_of(Builder& b, const char* what) {
    return std::string(what) + "_" + std::to_string(b.current_block()->parent()->next_block_id());
}

Value* tls(Builder& b) {
    Module* mod = b.current_block()->parent()->parent();
    return (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                               : b.build_call("bronze_tls_block_addr", Type::i64(), {});
}

// `code(env, this, argc, argv)`: through a register, or in a protected block
// through the module's trampoline (il_call_entry.h).
Value* enter_code(IlLowering* lowering, Builder& b, Value* code, Value* env, Value* this_val, uint32_t argc,
                  Value* argv) {
    Value* n = b.build_iconst_i32(static_cast<int32_t>(argc));
    if (lowering->in_protected_block()) {
        return b.build_call(lowering->module_sym(kEnterCodeTrampoline), Type::i64(), {code, env, this_val, n, argv});
    }
    return b.build_call_indirect(code, Type::i64(), {env, this_val, n, argv});
}

// The callee test as branches: `callee` an object, its header a function's,
// then `(flags word & mask) == realm | bits`, or with `native_argc` given, the
// native form of a call (kNativeCallBits, arity <= argc) once that fails.
// Leaves the builder where a test held and returns the function's payload;
// each failure goes to `miss`. A non-function's words past its header are
// never read.
Value* function_entry_test(Builder& b, Value* callee, uint64_t mask, uint64_t bits, BasicBlock* miss,
                           Value* native_argc = nullptr) {
    const std::string p = prefix_of(b, "fntest");
    BasicBlock* cur = b.current_block();
    BasicBlock* obj = b.append_block(p + "_obj");
    BasicBlock* fn = b.append_block(p + "_fn");
    BasicBlock* native = native_argc ? b.append_block(p + "_native") : nullptr;
    BasicBlock* ok = b.append_block(p + "_ok");
    b.position_at_end(cur);
    Value* ptr = b.build_xor(callee, imm(b, static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(ptr, imm(b, kPayloadLimit)), obj, miss);
    b.position_at_end(obj);
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kFunctionHeaderLow)), fn, miss);
    b.position_at_end(fn);
    Value* word = b.build_load(Type::i64(), ptr, BRONZE_ABI_FN_FLAGS_WORD_OFFSET);
    Value* realm = b.build_shl(b.build_load(Type::i64(), tls(b), BRONZE_TLS_REALM_SLOT_OFF),
                               imm(b, BRONZE_ABI_FN_REALM_SHIFT));
    Value* masked = b.build_and(word, imm(b, static_cast<int64_t>(mask)));
    Value* flags_ok = b.build_eq(masked, b.build_or(realm, imm(b, static_cast<int64_t>(bits))));
    b.build_br_if(flags_ok, ok, native ? native : miss);
    if (native) {
        b.position_at_end(native);
        Value* arity = b.build_load(Type::i32(), ptr, BRONZE_ABI_FN_ARITY_OFFSET);
        Value* native_ok = b.build_and(b.build_eq(masked, imm(b, static_cast<int64_t>(kNativeCallBits))),
                                       b.build_ule(arity, native_argc));
        b.build_br_if(native_ok, ok, miss);
    }
    b.position_at_end(ok);
    return ptr;
}

// A function payload's code and env, entered.
// In a build made to be harvested, a call or `new` with a slow site of its own
// keeps the callee it entered (il_property.h count_callee_turn).
Value* enter_function(IlLowering* lowering, Builder& b, Value* fn, Value* this_val, uint32_t argc, Value* argv) {
    Value* code = b.build_load(Type::ptr(), fn, BRONZE_ABI_FN_CODE_OFFSET);
    lowering->prop_lowering().count_callee_turn(b, b.build_load(Type::i64(), fn, BRONZE_ABI_FN_CODE_OFFSET));
    Value* env = b.build_load(Type::i64(), fn, BRONZE_ABI_FN_ENV_OFFSET);
    return enter_code(lowering, b, code, env, this_val, argc, argv);
}

}  // namespace

Value* direct_callee(Builder& b, Value* callee, Value* argc, BasicBlock* miss) {
    return function_entry_test(b, callee, kCallMask, kCallBits, miss, argc);
}

Value* call_function_code(Builder& b, Value* fn, Value* this_val, Value* argc, Value* argv) {
    Value* code = b.build_load(Type::ptr(), fn, BRONZE_ABI_FN_CODE_OFFSET);
    Value* env = b.build_load(Type::i64(), fn, BRONZE_ABI_FN_ENV_OFFSET);
    return b.build_call_indirect(code, Type::i64(), {env, this_val, argc, argv});
}

void build_enter_code_trampoline(Function* fn) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    Value* code = b.add_block_param(entry, Type::ptr());
    Value* env = b.add_block_param(entry, Type::i64());
    Value* this_val = b.add_block_param(entry, Type::i64());
    Value* argc = b.add_block_param(entry, Type::i32());
    Value* argv = b.add_block_param(entry, Type::ptr());
    b.position_at_end(entry);
    b.build_ret(b.build_call_indirect(code, Type::i64(), {env, this_val, argc, argv}));
    fn->rebuild_cfg_predecessors();
}

Value* lower_call_dynamic_inline(IlLowering* lowering, Builder& b, Value* callee, Value* this_val,
                                 const std::vector<Value*>& args) {
    const uint32_t argc = static_cast<uint32_t>(args.size());
    Value* argv = lowering->stage_argv(b, args);
    const std::string p = prefix_of(b, "call");
    BasicBlock* cur = b.current_block();
    BasicBlock* slow = b.append_block(p + "_slow");
    BasicBlock* merge = b.append_block(p + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());
    slow->set_cold();

    b.position_at_end(cur);
    Value* fn = function_entry_test(b, callee, kCallMask, kCallBits, slow, b.build_iconst_i32(static_cast<int32_t>(argc)));
    b.build_br(merge, {enter_function(lowering, b, fn, this_val, argc, argv)});

    b.position_at_end(slow);
    lowering->prop_lowering().slow_arm(b);
    b.build_br(merge, {b.build_call("bronze_call_dynamic_n", Type::i64(),
                                    {callee, this_val, b.build_iconst_i32(static_cast<int32_t>(argc)), argv})});
    b.position_at_end(merge);
    return result;
}

// The method site's words (bronze_abi_layout.h, the method-site contract)
// read at the call: a plain receiver of way 0's shape and
//   - a DIRECT entry (arity word bit 31, no slot form) of the thread's
//     epoch, latched for a callee that takes `argc` arguments without
//     padding: its code and env words entered;
//   - a SLOT entry (the arity word's high half the own slot plus one): the
//     callee read from the receiver's slot, then entered as a call enters
//     it.
// Everything else — way 1, the thread's megamorphic table, the EXOTIC and
// PRIMITIVE forms, a function receiver, an unfilled site — is the module's
// method stub (il_prop_stubs.cpp), with bronze_call_method behind it.
Value* lower_method_call_inline(IlLowering* lowering, Builder& b, Value* recv, Value* key_id,
                                const std::vector<Value*>& args, uint32_t ic_index) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    const IcSite site = pl.ic_site_ref(b, ic_index);
    const uint32_t argc = static_cast<uint32_t>(args.size());
    Value* argv = lowering->stage_argv(b, args);
    const std::string p = prefix_of(b, "mcall");
    BasicBlock* cur = b.current_block();
    BasicBlock* object = b.append_block(p + "_object");
    BasicBlock* way0 = b.append_block(p + "_way0");
    BasicBlock* direct = b.append_block(p + "_direct");
    BasicBlock* slot_test = b.append_block(p + "_slot_test");
    BasicBlock* slot = b.append_block(p + "_slot");
    BasicBlock* helper = b.append_block(p + "_helper");
    BasicBlock* merge = b.append_block(p + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());
    helper->set_cold();
    auto word = [&](int32_t w) { return b.build_load(Type::i64(), site.base, site.off + w * kWord); };

    b.position_at_end(cur);
    b.build_br_if(b.build_eq(b.build_lshr(recv, imm(b, 48)), imm(b, BRONZE_ABI_TAG_OBJECT)), object, helper);

    b.position_at_end(object);
    Value* ptr = b.build_lshr(b.build_shl(recv, imm(b, 16)), imm(b, 16));
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kPlainHeaderLow)), way0, helper);

    b.position_at_end(way0);
    b.build_br_if(b.build_eq(b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET), word(0)), direct, helper);

    // `(arity word ^ bit 31) <= argc`, unsigned, is the whole DIRECT form
    // test: no slot form, the direct bit, and no padding owed. The latch
    // records arity 0 for a compiled callee (rt_method_call.cpp,
    // directArityWord), whose wrapper pads itself.
    b.position_at_end(direct);
    Value* arity_word = word(BRONZE_ABI_METHOD_IC_ARITY_WORD);
    Value* entry_ok = b.build_ule(b.build_xor(arity_word, imm(b, static_cast<int64_t>(BRONZE_ABI_METHOD_IC_DIRECT_ENTRY_BIT))),
                                  imm(b, argc));
    Value* epoch_ok = b.build_eq(word(BRONZE_ABI_METHOD_IC_EPOCH_WORD),
                                 b.build_load(Type::i64(), tls(b), BRONZE_TLS_PROTO_EPOCH_OFF));
    BasicBlock* enter = b.append_block(p + "_enter");
    b.position_at_end(direct);
    b.build_br_if(b.build_and(entry_ok, epoch_ok), enter, slot_test);
    b.position_at_end(enter);
    {
        Value* code = b.build_load(Type::ptr(), site.base, site.off + BRONZE_ABI_METHOD_IC_CODE_WORD * kWord);
        Value* env = word(BRONZE_ABI_METHOD_IC_ENV_WORD);
        b.build_br(merge, {enter_code(lowering, b, code, env, recv, argc, argv)});
    }

    // Re-read at every call: receivers of one shape may hold different
    // functions in the slot.
    b.position_at_end(slot_test);
    b.build_br_if(b.build_uge(arity_word, imm(b, int64_t{1} << BRONZE_ABI_METHOD_IC_SLOT_SHIFT)), slot, helper);
    b.position_at_end(slot);
    {
        Value* slot_word = b.build_sub(b.build_lshr(arity_word, imm(b, BRONZE_ABI_METHOD_IC_SLOT_SHIFT)), imm(b, 1));
        Value* callee = pl.load_slot_br(b, ptr, slot_word);
        Value* fn = function_entry_test(b, callee, kCallMask, kCallBits, helper,
                                        b.build_iconst_i32(static_cast<int32_t>(argc)));
        b.build_br(merge, {enter_function(lowering, b, fn, recv, argc, argv)});
    }

    b.position_at_end(helper);
    pl.slow_arm(b);
    b.build_br(merge, {b.build_call(lowering->module_sym(kMethodCallStub), Type::i64(),
                                    {recv, key_id, b.build_iconst_i32(static_cast<int32_t>(argc)), argv,
                                     pl.site_ptr(b, site)})});
    b.position_at_end(merge);
    return result;
}

// `new`: a vetted function with the direct-call bit and an instance shape
// gets its instance bump-allocated here (what bronze_construct's plain path
// makes) and its code entered with it as `this`; an object it returns
// replaces the instance (10.2.2 step 10). Everything else is bronze_construct.
Value* lower_construct_inline(IlLowering* lowering, Builder& b, Value* ctor, const std::vector<Value*>& args) {
    const uint32_t argc = static_cast<uint32_t>(args.size());
    Value* argv = lowering->stage_argv(b, args);
    const std::string p = prefix_of(b, "new");
    BasicBlock* cur = b.current_block();
    BasicBlock* fast = b.append_block(p + "_fast");
    BasicBlock* slow = b.append_block(p + "_slow");
    BasicBlock* merge = b.append_block(p + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());
    slow->set_cold();

    b.position_at_end(cur);
    Value* fn = function_entry_test(b, ctor, kConstructMask, kConstructBits, slow);
    // The instance shape the runtime made (zero until the function's
    // prototype exists); its bare header sizes the instance.
    Value* shape = b.build_load(Type::i64(), fn, BRONZE_ABI_FN_INSTANCE_SHAPE_OFFSET);
    Value* obj_bits =
        lowering->alloc_lowering().bump_plain_object(b, shape, b.build_ne(shape, imm(b, 0)), slow, fast);
    // The instance is held tagged across the call, which may collect, and
    // read back after it for the result rule.
    Value* obj = lowering->ensure_type(obj_bits, Type::tagged(), b);
    Value* ret = enter_function(lowering, b, fn, obj_bits, argc, argv);
    Value* self = lowering->unbox_tagged(obj, b);
    Value* ret_is_obj = b.build_ult(b.build_xor(ret, imm(b, static_cast<int64_t>(kObjectTagBits))), imm(b, kPayloadLimit));
    b.build_br(merge, {b.build_select(ret_is_obj, ret, self)});

    b.position_at_end(slow);
    lowering->prop_lowering().slow_arm(b);
    b.build_br(merge, {b.build_call("bronze_construct", Type::i64(),
                                    {ctor, b.build_iconst_i32(static_cast<int32_t>(argc)), argv})});
    b.position_at_end(merge);
    return result;
}

}  // namespace il2mir
