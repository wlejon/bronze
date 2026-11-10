// The helpers il_direct_common.h declares: the stubs' shared tests, scans and
// walks.
#include "il_direct_common.h"

#include <brass/mir/block.hpp>
#include <brass/mir/function.hpp>

namespace il2mir::direct {
Value* tls_block(Builder& b) {
    Module* mod = b.current_block()->parent()->parent();
    return (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                               : b.build_call("bronze_tls_block_addr", Type::i64(), {});
}

std::string block_prefix(Builder& b, const char* what) {
    return std::string(what) + "_" + std::to_string(b.current_block()->parent()->next_block_id());
}

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

// entry_test as branches: a callee that is no object, or no function, goes to
// `miss` before anything past its header is read, and `ok` is the flags word's
// test alone, made where both held; `fn` is the payload. A guard that ends in
// a branch anyway pays two compares and branches for what the selects to the
// stand-in cost it.
EntryTest entry_test_br(Builder& b, Value* tls, Value* callee, uint64_t mask, uint64_t bits, BasicBlock* miss) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = block_prefix(b, "fntest");
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_fn = b.append_block(prefix + "_fn");
    b.position_at_end(cur);
    Value* ptr = b.build_xor(callee, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(ptr, b.build_iconst_i64(kPayloadLimit)), bb_obj, miss);
    b.position_at_end(bb_obj);
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kFunctionHeaderLow)), bb_fn, miss);
    b.position_at_end(bb_fn);
    Value* word = b.build_load(Type::i64(), ptr, BRONZE_ABI_FN_FLAGS_WORD_OFFSET);
    Value* realm = b.build_shl(b.build_load(Type::i64(), tls, BRONZE_TLS_REALM_SLOT_OFF),
                               b.build_iconst_i64(BRONZE_ABI_FN_REALM_SHIFT));
    Value* want = b.build_or(realm, b.build_iconst_i64(static_cast<int64_t>(bits)));
    return EntryTest{b.build_eq(b.build_and(word, b.build_iconst_i64(static_cast<int64_t>(mask))), want), ptr};
}

Value* call_code(Builder& b, Value* code, Value* env, Value* this_val, Value* argc, Value* argv) {
    return b.build_call_indirect(code, Type::i64(), {env, this_val, argc, argv});
}

Value* call_code(Builder& b, Value* code, Value* env, Value* this_val, uint32_t argc, Value* argv) {
    return call_code(b, code, env, this_val, b.build_iconst_i32(static_cast<int32_t>(argc)), argv);
}

// The scan of ways [first, ways) for the receiver `e` already describes.
void scan_ways(Builder& b, PropEntry& e, Value* site, int first, int ways) {
    Value* any = nullptr;
    Value* epoch = nullptr;
    e.slot_word = nullptr;
    // At most one way names a shape (an install rewrites an entry in place),
    // so the words select on the match alone.
    for (int32_t way = ways - 1; way >= first; --way) {
        const int32_t at = way * static_cast<int32_t>(BRONZE_ABI_IC_ENTRY_SIZE);
        Value* match = b.build_eq(e.shape, b.build_load(Type::i64(), site, at + BRONZE_ABI_IC_SHAPE_OFFSET));
        Value* word = b.build_load(Type::i64(), site, at + BRONZE_ABI_IC_SLOT_OFFSET);
        Value* ep = b.build_load(Type::i64(), site, at + BRONZE_ABI_IC_EPOCH_OFFSET);
        e.slot_word = e.slot_word ? b.build_select(match, word, e.slot_word) : word;
        epoch = epoch ? b.build_select(match, ep, epoch) : ep;
        any = any ? b.build_or(any, match) : match;
    }
    e.hit = e.plain ? b.build_and(e.plain, any) : any;
    e.epoch = epoch;
}

// The receiver as the stubs read it: its keyed shape, the object and header
// tests as two predicted branches with no select in the address the header and
// shape are read through, and its payload, read through only on a hit.
void keyed_receiver(PropertyLoweringHelper& pl, Builder& b, Value* obj, PropEntry& e) {
    e.shape = pl.keyed_shape(b, obj);
    e.plain = nullptr;
    e.ptr = b.build_xor(obj, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
}

PropEntry prop_entry(PropertyLoweringHelper& pl, Builder& b, Value* obj, Value* site, int ways) {
    PropEntry e;
    keyed_receiver(pl, b, obj, e);
    scan_ways(b, e, site, 0, ways);
    return e;
}

// The probe of the thread's megamorphic table (bronze_abi_tls.h, mega_prop)
// for the receiver `e` names under `key` (a key index, with the set bit for a
// write): branches to `hit` with the entry's slot word and epoch — an entry is
// a way of the same layout, so the caller reads it exactly as a way — or to
// `miss`. The index is the runtime's (rt_prop_mega.cpp), the ABI's formula.
//
// Only a FULL site probes: its last way holds a shape (an install fills way 0
// and shifts the rest down, so the last is the final one taken). A site with
// room still installs through the helper, so a site that sees one shape warms
// to its inline way even when another site already filled the table for that
// pair; an answer from the table would leave it calling the stub forever.
void build_mega_probe_if(Builder& b, Value* tls, const PropEntry& e, Value* full, Value* key, BasicBlock* hit,
                         BasicBlock* miss) {
    const auto mul = static_cast<int64_t>(BRONZE_ABI_MEGA_PROP_HASH_MUL);
    Value* h = b.build_mul(b.build_xor(e.shape, b.build_mul(key, b.build_iconst_i64(mul))), b.build_iconst_i64(mul));
    Value* index = b.build_lshr(h, b.build_iconst_i64(64 - BRONZE_ABI_MEGA_PROP_BITS));
    static_assert(BRONZE_ABI_MEGA_PROP_ENTRY_WORDS * kWord == 32, "the index scales by a shift of 5");
    Value* at = b.build_add(tls, b.build_add(b.build_shl(index, b.build_iconst_i64(5)),
                                             b.build_iconst_i64(BRONZE_TLS_MEGA_PROP_OFF)));
    Value* match = b.build_and(
        e.plain ? b.build_and(e.plain, full) : full, b.build_and(b.build_eq(b.build_load(Type::i64(), at, BRONZE_ABI_IC_SHAPE_OFFSET), e.shape),
                             b.build_eq(b.build_load(Type::i64(), at, BRONZE_ABI_MEGA_PROP_KEY_WORD * kWord), key)));
    Value* word = b.build_load(Type::i64(), at, BRONZE_ABI_IC_SLOT_OFFSET);
    Value* epoch = b.build_load(Type::i64(), at, BRONZE_ABI_IC_EPOCH_OFFSET);
    b.build_br_if(match, hit, {word, epoch}, miss, {});
}

void build_mega_probe(Builder& b, Value* tls, const PropEntry& e, Value* site, int ways, Value* key,
                      BasicBlock* hit, BasicBlock* miss) {
    const int32_t last = (ways - 1) * static_cast<int32_t>(BRONZE_ABI_IC_ENTRY_SIZE);
    Value* full = b.build_ne(b.build_load(Type::i64(), site, last + BRONZE_ABI_IC_SHAPE_OFFSET), b.build_iconst_i64(0));
    build_mega_probe_if(b, tls, e, full, key, hit, miss);
}

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
//
// Straight-line, one copy per link up to kMaxStubDepth, rather than a loop:
// a loop's back edge carries an interrupt poll and its block parameters
// spilled across it, and that was a third of a prototype read through the
// stub. The stubs exist once per module, so the copies cost nothing at a
// site.
Value* walk_to_prototype(Builder& b, Value* shape, Value* depth, BasicBlock* slow,
                         const std::string& prefix) {
    // append_block moves the insertion point.
    BasicBlock* entry = b.current_block();
    BasicBlock* found = b.append_block(prefix + "_holder");
    Value* holder = b.add_block_param(found, Type::i64());
    b.position_at_end(entry);
    Value* links = b.build_and(depth, b.build_iconst_i64(~kAccessorDepth));
    Value* cur_shape = shape;
    for (int64_t link = 1; link <= kMaxStubDepth; ++link) {
        const std::string at = prefix + "_l" + std::to_string(link);
        BasicBlock* has_root = b.append_block(at + "_root");
        BasicBlock* is_obj = b.append_block(at + "_obj");
        BasicBlock* is_plain = b.append_block(at + "_proto");
        BasicBlock* next = link < kMaxStubDepth ? b.append_block(at + "_next") : nullptr;
        b.position_at_end(entry);
        Value* root = b.build_load(Type::i64(), cur_shape, BRONZE_ABI_SHAPE_ROOT_OFFSET);
        b.build_br_if(b.build_ne(root, b.build_iconst_i64(0)), has_root, slow);

        b.position_at_end(has_root);
        Value* proto = b.build_load(Type::i64(), root, BRONZE_ABI_SHAPE_PROTO_OFFSET);
        Value* pptr = b.build_xor(proto, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
        // Two predicted branches: no select in the address the header is
        // read through.
        b.build_br_if(b.build_ult(pptr, b.build_iconst_i64(kPayloadLimit)), is_obj, slow);
        b.position_at_end(is_obj);
        Value* plain = b.build_eq(b.build_load(Type::i32(), pptr, 0), b.build_iconst_i32(kPlainHeaderLow));
        b.build_br_if(plain, is_plain, slow);

        b.position_at_end(is_plain);
        Value* hshape = b.build_load(Type::i64(), pptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
        Value* not_dict =
            b.build_eq(b.build_load(Type::i64(), hshape, BRONZE_ABI_SHAPE_DICT_OFFSET), b.build_iconst_i64(0));
        if (!next) {
            // walkable_depth bounds the links at kMaxStubDepth, so the last
            // copy is the holder whenever its link is sound.
            b.build_br_if(not_dict, found, {pptr}, slow, {});
            break;
        }
        Value* here = b.build_eq(links, b.build_iconst_i64(link));
        b.build_br_if(not_dict, next, slow);
        b.position_at_end(next);
        BasicBlock* cont = b.append_block(at + "_cont");
        b.position_at_end(next);
        b.build_br_if(here, found, {pptr}, cont, {});
        entry = cont;
        cur_shape = hshape;
    }

    b.position_at_end(found);
    return holder;
}

}  // namespace il2mir::direct
