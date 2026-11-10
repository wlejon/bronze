// Native builtins entered at the method site (il_native_fast.h).
//
// The table: which member, of which receiver kind, for how many arguments,
// enters which code. A fast entry takes the receiver and `arity` arguments
// in registers, the site's padded with `undefined` (each member listed reads
// an absent argument and an undefined one alike: native_fast.h). An Array
// member without one is entered as its own code, (env, this, argc, argv),
// which is what the method cache's EXOTIC hit would enter it as, minus the
// stub and the dispatch around it.

#include "il_native_fast.h"

#include "il_abi.h"
#include "il_direct_common.h"
#include "il_lowering.h"
#include "il_lowering_direct.h"
#include "il_property.h"
#include "abi/bronze_abi.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace il2mir {

enum class NativeRecv : uint8_t { Array, String };

struct NativeFastRow {
    std::string_view key;
    NativeRecv recv;
    uint8_t min_args;
    uint8_t max_args;
    // The fast entry and its argument count; null `fast` enters `generic`
    // with an argv.
    const char* fast;
    uint8_t arity;
    // The member's own code: what a String row compares the slot's function
    // with, and what an Array row without a fast entry calls.
    const char* generic;
    bronze_fn_code code;
};

namespace {

using namespace direct;

constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr uint64_t kTagMask = 0xFFFF000000000000ULL;
// The PRIMITIVE form's word 0 for a String receiver (rt_method_call.cpp,
// latchPrimitiveMethodIc).
constexpr int64_t kStringPrimWord =
    (int64_t{BRONZE_ABI_TAG_STRING} << BRONZE_ABI_METHOD_IC_KIND_SHIFT) | int64_t{BRONZE_ABI_METHOD_IC_EXOTIC_BIT};

const NativeFastRow kRows[] = {
    {"indexOf", NativeRecv::Array, 1, 1, "bronze_fast_array_index_of", 1, nullptr, nullptr},
    {"includes", NativeRecv::Array, 1, 1, "bronze_fast_array_includes", 1, nullptr, nullptr},
    {"push", NativeRecv::Array, 1, 8, nullptr, 0, "bronze_array_push", &bronze_array_push},
    {"pop", NativeRecv::Array, 0, 0, nullptr, 0, "bronze_array_pop", &bronze_array_pop},
    {"charCodeAt", NativeRecv::String, 0, 1, "bronze_fast_string_char_code_at", 1, "bronze_string_char_code_at",
     &bronze_string_char_code_at},
    {"charAt", NativeRecv::String, 0, 1, "bronze_fast_string_char_at", 1, "bronze_string_char_at",
     &bronze_string_char_at},
    {"indexOf", NativeRecv::String, 1, 2, "bronze_fast_string_index_of", 2, "bronze_string_index_of",
     &bronze_string_index_of},
    {"startsWith", NativeRecv::String, 1, 2, "bronze_fast_string_starts_with", 2, "bronze_string_starts_with",
     &bronze_string_starts_with},
    {"slice", NativeRecv::String, 0, 2, "bronze_fast_string_slice", 2, "bronze_string_slice", &bronze_string_slice},
    {"substring", NativeRecv::String, 0, 2, "bronze_fast_string_substring", 2, "bronze_string_substring",
     &bronze_string_substring},
};

// Where a String row's code compare gets the member's address, as the Math
// builtins' guard does (il_lowering_calls.cpp, builtin_target_for_key): this
// process's for a tier compiled here, the import's for an image.
enum class CodeRef : uint8_t { None, Here, Image };

CodeRef code_ref(IlLowering* lowering) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    if (pl.feedback_driven() && pl.in_process()) return CodeRef::Here;
    if (!pl.in_process() && lowering->options().direct_js_calls) return CodeRef::Image;
    return CodeRef::None;
}

Value* iconst(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

Value* enter(IlLowering* lowering, Builder& b, const NativeFastRow& row, Value* recv,
             const std::vector<Value*>& args) {
    if (row.fast) {
        std::vector<Value*> operands = {recv};
        for (size_t i = 0; i < row.arity; ++i) operands.push_back(i < args.size() ? args[i] : iconst(b, kUndefinedTag));
        return b.build_call(row.fast, Type::i64(), operands);
    }
    Value* argv = lowering->stage_argv(b, args);
    return b.build_call(row.generic, Type::i64(),
                        {iconst(b, kUndefinedTag), recv, b.build_iconst_i32(static_cast<int32_t>(args.size())), argv});
}

// An Array with no named-property box (il_array_method.cpp's first test).
void array_guard(Builder& b, Value* recv, BasicBlock* hit, BasicBlock* miss, const std::string& prefix) {
    BasicBlock* cur = b.current_block();
    BasicBlock* bb_arr = b.append_block(prefix + "_array");
    b.position_at_end(cur);
    Value* ptr = b.build_xor(recv, iconst(b, kObjectTagBits));
    Value* is_obj = b.build_ult(ptr, b.build_iconst_i64(kPayloadLimit));
    // A non-object's header is read through the TLS block, whose first word
    // is no header (bronze_abi.h, proto_epoch).
    Value* base = b.build_select(is_obj, ptr, tls_block(b));
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kArrayHeaderLow)), bb_arr, miss);
    b.position_at_end(bb_arr);
    Value* props = b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
    b.build_br_if(b.build_ne(b.build_and(props, iconst(b, kTagMask)), iconst(b, kObjectTagBits)), hit, miss);
}

// A String, the site's way 0 the PRIMITIVE entry, `String.prototype` of its
// latched shape, and the function in the entry's slot this member's code.
void string_guard(IlLowering* lowering, Builder& b, const NativeFastRow& row, Value* recv, Value* site,
                  BasicBlock* hit, BasicBlock* miss, const std::string& prefix) {
    BasicBlock* cur = b.current_block();
    BasicBlock* bb_holder = b.append_block(prefix + "_holder");
    BasicBlock* bb_fn = b.append_block(prefix + "_fn");
    BasicBlock* bb_code = b.append_block(prefix + "_code");
    b.position_at_end(cur);
    Value* holder = b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_ENV_WORD * kWord);
    Value* is_str = b.build_eq(b.build_lshr(recv, b.build_iconst_i64(48)), b.build_iconst_i64(BRONZE_ABI_TAG_STRING));
    // The site's own words, not its method row: the entry's form is what
    // says the env and aux words name String.prototype and its shape.
    Value* is_prim = b.build_eq(b.build_load(Type::i64(), site, 0), b.build_iconst_i64(kStringPrimWord));
    Value* holder_obj = b.build_eq(b.build_lshr(holder, b.build_iconst_i64(48)), b.build_iconst_i64(BRONZE_ABI_TAG_OBJECT));
    b.build_br_if(b.build_and(b.build_and(is_str, is_prim), holder_obj), bb_holder, miss);

    b.position_at_end(bb_holder);
    Value* hptr = b.build_xor(holder, iconst(b, kObjectTagBits));
    Value* arity = b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_ARITY_WORD * kWord);
    Value* same = b.build_and(
        b.build_eq(b.build_load(Type::i32(), hptr, 0), b.build_iconst_i32(kPlainHeaderLow)),
        b.build_eq(b.build_load(Type::i64(), hptr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
                   b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_AUX_WORD * kWord)));
    b.build_br_if(b.build_and(same, b.build_uge(arity, b.build_iconst_i64(kSlotFormLimit))), bb_fn, miss);

    b.position_at_end(bb_fn);
    Value* slot_word =
        b.build_sub(b.build_lshr(arity, b.build_iconst_i64(BRONZE_ABI_METHOD_IC_SLOT_SHIFT)), b.build_iconst_i64(1));
    Value* fn = lowering->prop_lowering().load_slot(b, hptr, slot_word);
    Value* fptr = b.build_xor(fn, iconst(b, kObjectTagBits));
    b.build_br_if(b.build_ult(fptr, b.build_iconst_i64(kPayloadLimit)), bb_code, miss);

    b.position_at_end(bb_code);
    Value* is_fn = b.build_eq(b.build_load(Type::i32(), fptr, 0), b.build_iconst_i32(kFunctionHeaderLow));
    Value* is_code = code_ref(lowering) == CodeRef::Here
        ? b.build_eq(b.build_load(Type::i64(), fptr, BRONZE_ABI_FN_CODE_OFFSET),
                     iconst(b, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(row.code))))
        : b.build_eq(b.build_load(Type::ptr(), fptr, BRONZE_ABI_FN_CODE_OFFSET), b.build_func_addr(row.generic));
    b.build_br_if(b.build_and(is_fn, is_code), hit, miss);
}

}  // namespace

std::vector<const NativeFastRow*> native_fast_rows(IlLowering* lowering, const BronzeInstruction& inst) {
    std::vector<const NativeFastRow*> out;
    if (!lowering || !direct_method_ic_here(lowering) || lowering->run_once()) return out;
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    if (!pl.enable_inlined_fastpaths() || inst.ic_index == BronzeInstruction::kNoIcIndex) return out;
    const std::string_view key = pl.key_tag(inst.index);
    const bool strings = code_ref(lowering) != CodeRef::None;
    for (const NativeFastRow& row : kRows) {
        if (row.key != key || inst.param_count < row.min_args || inst.param_count > row.max_args) continue;
        if (row.recv == NativeRecv::String && !strings) continue;
        out.push_back(&row);
    }
    return out;
}

Value* lower_native_fast(IlLowering* lowering, Builder& b, const std::vector<const NativeFastRow*>& rows,
                         Value* recv, const std::vector<Value*>& boxed_args, Value* site,
                         const std::function<Value*(Builder&)>& slow) {
    const std::string prefix = block_prefix(b, "native");
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());
    for (size_t i = 0; i < rows.size(); ++i) {
        const NativeFastRow& row = *rows[i];
        const std::string arm = prefix + "_" + std::to_string(i);
        BasicBlock* hit = b.append_block(arm + "_enter");
        BasicBlock* miss = b.append_block(arm + "_miss");
        if (i + 1 == rows.size()) miss->set_cold();
        b.position_at_end(cur);
        if (row.recv == NativeRecv::Array) {
            array_guard(b, recv, hit, miss, arm);
        } else {
            string_guard(lowering, b, row, recv, site, hit, miss, arm);
        }
        b.position_at_end(hit);
        b.build_br(merge, {enter(lowering, b, row, recv, boxed_args)});
        b.position_at_end(miss);
        cur = miss;
    }
    const size_t slow_base = fn->blocks().size();
    b.build_br(merge, {slow(b)});
    for (size_t bi = slow_base; bi < fn->blocks().size(); ++bi) fn->blocks()[bi]->set_cold();
    b.position_at_end(merge);
    return result;
}

}  // namespace il2mir
