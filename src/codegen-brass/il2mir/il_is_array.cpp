// `X.isArray(v)` answered at the site while the site's method cache holds the
// global constructor static `Array.isArray` (rt_method_call.cpp,
// latchExoticMethodIc): the EXOTIC form with the CODE guard on a Function
// receiver, whose aux word is the receiver's code pointer. The latch's witness
// (rtGlobalConstructorStaticMatches) found the table's own `isArray` for that
// constructor and this key, and `Array` is the one constructor with an
// `isArray` static, so a receiver whose code word is the aux word reaches
// 23.1.2.2 itself for as long as the entry is good: while its statics are
// still the table's (an entry latched then carries no epoch), or, once the
// program has written them into the box, while the epoch it was latched at is
// current (every write to them bumps it). Anything else — another receiver,
// a patched `Array.isArray`, a cold site — is the method stub.
//
// The answer, 7.2.2 IsArray, for the kinds that decide it without the
// runtime: a non-object is false; an Array is true; a function or a typed
// array is false. A plain object is false unless it is `Array.prototype`,
// which bronze keeps as a plain object (builtin_array.cpp). That object's
// [[Prototype]] is always `Object.prototype` (bronze refuses to replace it)
// and `Object.prototype`'s is always null (an immutable prototype exotic
// object), so a plain object whose prototype is null, or is a plain object
// that itself has a prototype, is not it: a class instance, the common
// receiver of `Array.isArray(material)`. The rest — a literal whose
// prototype is `Object.prototype`, a proxy, a dictionary — calls the stub.
#include "il_abi.h"
#include "il_is_array.h"
#include "il_lowering.h"
#include "il_lowering_direct.h"

#include <string>

namespace il2mir {

namespace {

constexpr int32_t kWord = 8;
// FunctionHeader::statics_materialized (pinned by a static_assert in
// runtime/fn.h).
constexpr int32_t kStaticsMaterializedOffset = BRONZE_ABI_FN_FLAGS_WORD_OFFSET + 4;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr int32_t kHeaderLow(uint32_t kind) { return static_cast<int32_t>((kind << 16) | BRONZE_ABI_TAG_OBJECT); }
// Word 0 of a constructor-static entry: the aux word is the receiver's code
// pointer, the kind a Function's, the CODE guard and the EXOTIC bit.
constexpr int64_t kStaticWord0 =
    (int64_t{BRONZE_ABI_FN_CODE_OFFSET} << BRONZE_ABI_METHOD_IC_BOX_SHIFT) |
    (int64_t{BRONZE_ABI_OBJ_FLAGS_FUNCTION} << BRONZE_ABI_METHOD_IC_KIND_SHIFT) |
    static_cast<int64_t>(BRONZE_ABI_METHOD_IC_CODE_GUARD_BIT | BRONZE_ABI_METHOD_IC_EXOTIC_BIT);

// The [[Prototype]] Value of the plain, non-dictionary object at `ptr`
// (shape -> root -> prototype, as the read stub's walk takes it), read in
// the current block's successors; `slow` for a dictionary or a shape with no
// root.
Value* plain_proto(Builder& b, Value* ptr, BasicBlock* slow, const std::string& prefix) {
    BasicBlock* cur = b.current_block();
    BasicBlock* rooted = b.append_block(prefix + "_rooted");
    BasicBlock* proto = b.append_block(prefix + "_proto");
    b.position_at_end(cur);
    Value* shape = b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    Value* not_dict = b.build_eq(b.build_load(Type::i64(), shape, BRONZE_ABI_SHAPE_DICT_OFFSET), b.build_iconst_i64(0));
    b.build_br_if(not_dict, rooted, slow);
    b.position_at_end(rooted);
    Value* root = b.build_load(Type::i64(), shape, BRONZE_ABI_SHAPE_ROOT_OFFSET);
    b.build_br_if(b.build_ne(root, b.build_iconst_i64(0)), proto, slow);
    b.position_at_end(proto);
    return b.build_load(Type::i64(), root, BRONZE_ABI_SHAPE_PROTO_OFFSET);
}

Value* untag_object(Builder& b, Value* v) {
    return b.build_xor(v, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
}

Value* is_object_ptr(Builder& b, Value* ptr) { return b.build_ult(ptr, b.build_iconst_i64(kPayloadLimit)); }

}  // namespace

Value* lower_is_array_call(IlLowering* lowering, const BronzeInstruction& inst, Builder& b, Value* recv,
                           const std::vector<Value*>& boxed_args, Value* site, bool poly_recv) {
    if (!lowering || !site || boxed_args.empty() || lowering->run_once() || !direct_method_ic_here(lowering) ||
        !lowering->prop_lowering().enable_inlined_fastpaths()) {
        return nullptr;
    }
    const auto& keys = lowering->options().key_constants;
    if (inst.index >= keys.size() || keys[inst.index] != "isArray") return nullptr;

    Function* fn = b.current_block()->parent();
    const std::string prefix = "isarray_" + std::to_string(fn->next_block_id());
    // append_block moves the insertion point: every block first.
    BasicBlock* cur = b.current_block();
    BasicBlock* latched_bb = b.append_block(prefix + "_latched");
    BasicBlock* recv_fn = b.append_block(prefix + "_recvfn");
    BasicBlock* code_bb = b.append_block(prefix + "_code");
    BasicBlock* boxed = b.append_block(prefix + "_box");
    BasicBlock* fresh = b.append_block(prefix + "_table");
    BasicBlock* arg_obj = b.append_block(prefix + "_arg");
    BasicBlock* not_array = b.append_block(prefix + "_notarray");
    BasicBlock* other_kind = b.append_block(prefix + "_kind");
    BasicBlock* plain = b.append_block(prefix + "_plain");
    BasicBlock* proto_obj = b.append_block(prefix + "_pobj");
    BasicBlock* proto_ptr = b.append_block(prefix + "_pptr");
    BasicBlock* proto_plain = b.append_block(prefix + "_pplain");
    BasicBlock* slow = b.append_block(prefix + "_slow_cold");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());
    slow->set_cold();

    // The site's entry: a constructor static latched for a receiver whose
    // code word is the aux word.
    b.position_at_end(cur);
    Value* yes = b.build_iconst_i64(static_cast<int64_t>(kBoolTag | 1));
    Value* no = b.build_iconst_i64(static_cast<int64_t>(kBoolTag));
    Value* rptr = untag_object(b, recv);
    Value* latched = b.build_and(b.build_eq(b.build_load(Type::i64(), site, 0), b.build_iconst_i64(kStaticWord0)),
                                 is_object_ptr(b, rptr));
    b.build_br_if(latched, latched_bb, slow);
    b.position_at_end(latched_bb);
    Value* is_fn =
        b.build_eq(b.build_load(Type::i32(), rptr, 0), b.build_iconst_i32(kHeaderLow(BRONZE_ABI_OBJ_FLAGS_FUNCTION)));
    b.build_br_if(is_fn, recv_fn, slow);
    b.position_at_end(recv_fn);
    Value* code_ok = b.build_eq(b.build_load(Type::i64(), rptr, BRONZE_ABI_FN_CODE_OFFSET),
                                b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_AUX_WORD * kWord));
    b.build_br_if(code_ok, code_bb, slow);
    // Statics still the table's: the entry needs no epoch.
    b.position_at_end(code_bb);
    Value* table = b.build_eq(
        b.build_zext_i64(b.build_load(Type::i8(), rptr, kStaticsMaterializedOffset)),
        b.build_iconst_i64(0));
    b.build_br_if(table, fresh, boxed);
    // Statics in the box: the entry's epoch is the thread's.
    b.position_at_end(boxed);
    Value* epoch = b.build_load(Type::i64(), b.build_pinned_tls_read(), BRONZE_TLS_PROTO_EPOCH_OFF);
    b.build_br_if(b.build_eq(b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_EPOCH_WORD * kWord), epoch), fresh,
                  slow);

    // 7.2.2 on the argument.
    b.position_at_end(fresh);
    Value* aptr = untag_object(b, boxed_args[0]);
    b.build_br_if(is_object_ptr(b, aptr), arg_obj, {}, merge, {no});
    b.position_at_end(arg_obj);
    Value* header = b.build_load(Type::i32(), aptr, 0);
    b.build_br_if(b.build_eq(header, b.build_iconst_i32(kHeaderLow(BRONZE_ABI_OBJ_FLAGS_ARRAY))), merge, {yes},
                  not_array, {});
    b.position_at_end(not_array);
    b.build_br_if(b.build_eq(header, b.build_iconst_i32(kHeaderLow(BRONZE_ABI_OBJ_FLAGS_PLAIN))), plain, other_kind);
    b.position_at_end(other_kind);
    Value* fn_or_view =
        b.build_or(b.build_eq(header, b.build_iconst_i32(kHeaderLow(BRONZE_ABI_OBJ_FLAGS_FUNCTION))),
                   b.build_eq(header, b.build_iconst_i32(kHeaderLow(BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY))));
    b.build_br_if(fn_or_view, merge, {no}, slow, {});
    // A plain object: its prototype null, or a plain object with one.
    b.position_at_end(plain);
    Value* proto = plain_proto(b, aptr, slow, prefix + "_a");
    b.build_br_if(b.build_eq(proto, b.build_iconst_i64(static_cast<int64_t>(kNullTag))), merge, {no}, proto_obj, {});
    b.position_at_end(proto_obj);
    Value* pptr = untag_object(b, proto);
    b.build_br_if(is_object_ptr(b, pptr), proto_ptr, slow);
    b.position_at_end(proto_ptr);
    b.build_br_if(
        b.build_eq(b.build_load(Type::i32(), pptr, 0), b.build_iconst_i32(kHeaderLow(BRONZE_ABI_OBJ_FLAGS_PLAIN))),
        proto_plain, slow);
    b.position_at_end(proto_plain);
    Value* grand = plain_proto(b, pptr, slow, prefix + "_p");
    b.build_br_if(is_object_ptr(b, untag_object(b, grand)), merge, {no}, slow, {});

    b.position_at_end(slow);
    Value* key_id = lowering->get_key_id(b, inst.index);
    Value* row = lowering->prop_lowering().method_row(b, inst.ic_index);
    b.build_br(merge, {lower_method_call_ic(lowering, b, recv, key_id, boxed_args, site, row, poly_recv)});

    b.position_at_end(merge);
    return result;
}

}  // namespace il2mir
