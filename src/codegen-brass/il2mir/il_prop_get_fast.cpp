// kPropGetStub (il_property.h): the read stub every cached read site calls.
//
// build_prop_get_stub (il_lowering_direct.cpp, kPropGetSlowStub) answers a
// site's read from its ways, the thread's megamorphic table and the prototype
// chain, and calls a getter or the helper for the rest. This is its hit paths
// without the calls: an own data slot, an absent key and a prototype's data
// slot, from way 0, the other ways or the table. Everything else (a getter, a
// receiver nothing names, an entry whose epoch has passed, a chain link that
// is not a plain object of a non-dictionary shape) goes to the full stub with
// the same operands, which answers the whole read again. On three_meshes 99%
// of the reads are answered here (docs/aot-specialization.md, S4).
//
// It is written for a leaf's registers. brass merges equal constants module
// wide and keeps the result live from its first definition, so a 64-bit mask
// used in two places held a register (or a stack slot) across the whole
// stub, and the stub saved callee-saved registers for it on every call. Here
// every tag, payload and half-word test is a shift and a compare against an
// immediate, each way is a compare and a branch of its own, and each answer
// returns from its own block.
//
// The site calls this one function, not a call-free half and then the full
// stub on its miss: that split (the half declared nocollect, so the caller's
// references stayed in registers across it) gave the same stub savings but
// cost the callers more than it saved, a second call per site, the site's
// operands kept across the first, and 11% more image code.
#include "il_prop_get_fast.h"

#include "il_abi.h"
#include "il_lowering.h"
#include "il_property.h"

#include <string>
#include <vector>

namespace il2mir {

namespace {

constexpr int32_t kPlainHeader = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int64_t kMaxDepth = 4;  // build_prop_get_stub's kMaxStubDepth

Value* imm(Builder& b, int64_t v) { return b.build_iconst_i64(v); }

// The low 48 bits of a word (a payload, or the overflow block's address).
Value* payload(Builder& b, Value* word) { return b.build_lshr(b.build_shl(word, imm(b, 16)), imm(b, 16)); }

// Whether `word` is a Value with the object tag.
Value* object_tagged(Builder& b, Value* word) {
    return b.build_eq(b.build_lshr(word, imm(b, 48)), imm(b, BRONZE_ABI_TAG_OBJECT));
}

// PropertyLoweringHelper::load_slot with the overflow block's tag shifted off.
Value* load_slot(Builder& b, Value* ptr, Value* slot) {
    Value* is_inline = b.build_ult(slot, imm(b, kBronzeObjInlineSlots));
    Value* overflow = payload(b, b.build_load(Type::i64(), ptr, kBronzeObjOverflowOffset));
    constexpr int64_t kShift = kBronzeObjSlotsOffset + kBronzeObjInlineSlots * 8 - kBronzeHdrBytes;
    Value* base = b.build_select(is_inline, ptr, b.build_sub(overflow, imm(b, kShift)));
    return b.build_load_indexed(Type::i64(), base, slot, 8, kBronzeObjSlotsOffset);
}

}  // namespace

void build_prop_get_fast_stub(Function* fn, bool to_front) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    Value* obj = b.add_block_param(entry, Type::i64());
    Value* key_id = b.add_block_param(entry, Type::i32());
    Value* site = b.add_block_param(entry, Type::ptr());
    const std::string p = "pfast";
    BasicBlock* miss = b.append_block(p + "_miss");
    BasicBlock* object = b.append_block(p + "_object");
    BasicBlock* hit = b.append_block(p + "_hit");
    Value* word = b.add_block_param(hit, Type::i64());
    Value* word_epoch = b.add_block_param(hit, Type::i64());
    BasicBlock* own = b.append_block(p + "_own");
    BasicBlock* not_own = b.append_block(p + "_notown");
    BasicBlock* chain = b.append_block(p + "_chain");
    BasicBlock* absent = b.append_block(p + "_absent");
    BasicBlock* walk = b.append_block(p + "_walk");
    BasicBlock* holder_found = b.append_block(p + "_holder");
    Value* holder = b.add_block_param(holder_found, Type::i64());
    BasicBlock* mega = b.append_block(p + "_mega");
    std::vector<BasicBlock*> ways;
    for (int w = 0; w < BRONZE_ABI_IC_WAYS; ++w) ways.push_back(b.append_block(p + "_way" + std::to_string(w)));

    // A plain object: the object tag, then the header's flags.
    b.position_at_end(entry);
    b.build_br_if(object_tagged(b, obj), object, miss);
    b.position_at_end(object);
    Value* ptr = payload(b, obj);
    Value* shape = b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kPlainHeader)), ways[0], miss);

    // Its shape against each way; an entry is (shape, slot word, epoch).
    for (int w = 0; w < BRONZE_ABI_IC_WAYS; ++w) {
        const int32_t at = w * static_cast<int32_t>(BRONZE_ABI_IC_ENTRY_SIZE);
        BasicBlock* found = b.append_block(p + "_found" + std::to_string(w));
        b.position_at_end(ways[w]);
        Value* way_shape = b.build_load(Type::i64(), site, at + BRONZE_ABI_IC_SHAPE_OFFSET);
        BasicBlock* next = ways[w];
        if (w + 1 < BRONZE_ABI_IC_WAYS) {
            next = ways[w + 1];
            b.build_br_if(b.build_eq(shape, way_shape), found, next);
        } else {
            // The last way empty: the site still installs through the helper
            // and does not probe the table (build_mega_probe says why).
            BasicBlock* full = b.append_block(p + "_full");
            b.position_at_end(ways[w]);
            b.build_br_if(b.build_eq(shape, way_shape), found, full);
            b.position_at_end(full);
            b.build_br_if(b.build_eq(way_shape, imm(b, 0)), miss, mega);
        }
        b.position_at_end(found);
        b.build_br(hit, {b.build_load(Type::i64(), site, at + BRONZE_ABI_IC_SLOT_OFFSET),
                         b.build_load(Type::i64(), site, at + BRONZE_ABI_IC_EPOCH_OFFSET)});
    }

    // The thread's (shape, key) table, indexed as rt_prop_mega.cpp indexes it.
    b.position_at_end(mega);
    {
        Value* tls = b.build_pinned_tls_read();
        Value* key = b.build_zext_i64(key_id);
        const auto mul = static_cast<int64_t>(BRONZE_ABI_MEGA_PROP_HASH_MUL);
        Value* h = b.build_mul(b.build_xor(shape, b.build_mul(key, imm(b, mul))), imm(b, mul));
        Value* index = b.build_lshr(h, imm(b, 64 - BRONZE_ABI_MEGA_PROP_BITS));
        static_assert(BRONZE_ABI_MEGA_PROP_ENTRY_WORDS * 8 == 32, "the index scales by a shift of 5");
        Value* at = b.build_add(tls, b.build_add(b.build_shl(index, imm(b, 5)), imm(b, BRONZE_TLS_MEGA_PROP_OFF)));
        Value* match = b.build_and(b.build_eq(b.build_load(Type::i64(), at, BRONZE_ABI_IC_SHAPE_OFFSET), shape),
                                   b.build_eq(b.build_load(Type::i64(), at, BRONZE_ABI_MEGA_PROP_KEY_WORD * 8), key));
        Value* mword = b.build_load(Type::i64(), at, BRONZE_ABI_IC_SLOT_OFFSET);
        Value* mepoch = b.build_load(Type::i64(), at, BRONZE_ABI_IC_EPOCH_OFFSET);
        if (!to_front) {
            b.build_br_if(match, hit, {mword, mepoch}, miss, {});
        } else {
            // An entry is about its (shape, key) pair alone (rt_prop_mega.h),
            // so the table's is this site's too: installed at way 0 as the
            // runtime installs one (InlineCacheSite::install, move to front),
            // and left out when way 0 holds an array-method sentinel, which
            // stays there.
            BasicBlock* front = b.append_block(p + "_front");
            BasicBlock* shift = b.append_block(p + "_shift");
            b.position_at_end(mega);
            b.build_br_if(match, front, miss);
            b.position_at_end(front);
            Value* sentinel = b.build_eq(b.build_load(Type::i64(), site, BRONZE_ABI_IC_SHAPE_OFFSET),
                                         imm(b, static_cast<int64_t>(BRONZE_ABI_IC_SHAPE_ARRAY_METHOD)));
            b.build_br_if(sentinel, hit, {mword, mepoch}, shift, {});
            b.position_at_end(shift);
            constexpr int32_t kEntry = static_cast<int32_t>(BRONZE_ABI_IC_ENTRY_SIZE);
            static_assert(BRONZE_ABI_IC_ENTRY_SIZE == 24, "an entry is three words");
            for (int w = BRONZE_ABI_IC_WAYS - 1; w > 0; --w) {
                for (int32_t off = 0; off < kEntry; off += 8) {
                    b.build_store(Type::i64(), site, w * kEntry + off, b.build_load(Type::i64(), site, (w - 1) * kEntry + off));
                }
            }
            b.build_store(Type::i64(), site, BRONZE_ABI_IC_SHAPE_OFFSET, shape);
            b.build_store(Type::i64(), site, BRONZE_ABI_IC_SLOT_OFFSET, mword);
            b.build_store(Type::i64(), site, BRONZE_ABI_IC_EPOCH_OFFSET, mepoch);
            b.build_br(hit, {mword, mepoch});
        }
    }

    // The slot word is (depth << 32) | slot, the flags in the depth half.
    b.position_at_end(hit);
    Value* depth = b.build_lshr(word, imm(b, 32));
    b.build_br_if(b.build_eq(depth, imm(b, 0)), own, not_own);

    b.position_at_end(own);
    b.build_ret(load_slot(b, ptr, word));

    // Every other answer holds while the entry's epoch is the thread's.
    b.position_at_end(not_own);
    Value* epoch = b.build_load(Type::i64(), b.build_pinned_tls_read(), BRONZE_TLS_PROTO_EPOCH_OFF);
    b.build_br_if(b.build_eq(word_epoch, epoch), chain, miss);

    b.position_at_end(chain);
    b.build_br_if(b.build_eq(depth, imm(b, BRONZE_ABI_IC_DEPTH_ABSENT_FLAG)), absent, walk);

    b.position_at_end(absent);
    b.build_ret(imm(b, static_cast<int64_t>(kUndefinedTag)));

    // A data entry 1..kMaxDepth links up: any flag (accessor, absent, f64)
    // puts depth - 1 far past the bound. Each link is what
    // ObjectHeader::cachedProtoHolder asks of it: the shape's root, a plain
    // object as the root's prototype, and that object's shape no dictionary.
    b.position_at_end(walk);
    {
        BasicBlock* link = b.append_block(p + "_l1");
        b.position_at_end(walk);
        b.build_br_if(b.build_ult(b.build_sub(depth, imm(b, 1)), imm(b, kMaxDepth)), link, miss);
        Value* cur_shape = shape;
        for (int64_t n = 1; n <= kMaxDepth; ++n) {
            const std::string at = p + "_l" + std::to_string(n);
            BasicBlock* has_root = b.append_block(at + "_root");
            BasicBlock* proto_obj = b.append_block(at + "_obj");
            BasicBlock* proto_plain = b.append_block(at + "_plain");
            BasicBlock* next = n < kMaxDepth ? b.append_block(p + "_l" + std::to_string(n + 1)) : nullptr;
            b.position_at_end(link);
            Value* root = b.build_load(Type::i64(), cur_shape, BRONZE_ABI_SHAPE_ROOT_OFFSET);
            b.build_br_if(b.build_ne(root, imm(b, 0)), has_root, miss);
            b.position_at_end(has_root);
            Value* proto = b.build_load(Type::i64(), root, BRONZE_ABI_SHAPE_PROTO_OFFSET);
            b.build_br_if(object_tagged(b, proto), proto_obj, miss);
            b.position_at_end(proto_obj);
            Value* pptr = payload(b, proto);
            b.build_br_if(b.build_eq(b.build_load(Type::i32(), pptr, 0), b.build_iconst_i32(kPlainHeader)),
                          proto_plain, miss);
            b.position_at_end(proto_plain);
            Value* pshape = b.build_load(Type::i64(), pptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
            Value* not_dict = b.build_eq(b.build_load(Type::i64(), pshape, BRONZE_ABI_SHAPE_DICT_OFFSET), imm(b, 0));
            if (!next) {
                b.build_br_if(not_dict, holder_found, {pptr}, miss, {});
                break;
            }
            BasicBlock* here = b.append_block(at + "_here");
            b.position_at_end(proto_plain);
            b.build_br_if(not_dict, here, miss);
            b.position_at_end(here);
            b.build_br_if(b.build_eq(depth, imm(b, n)), holder_found, {pptr}, next, {});
            link = next;
            cur_shape = pshape;
        }
    }

    b.position_at_end(holder_found);
    b.build_ret(load_slot(b, holder, b.build_lshr(b.build_shl(word, imm(b, 32)), imm(b, 32))));

    b.position_at_end(miss);
    b.build_ret(b.build_call(kPropGetSlowStub, Type::i64(), {obj, key_id, site}));
    fn->rebuild_cfg_predecessors();
}

}  // namespace il2mir
