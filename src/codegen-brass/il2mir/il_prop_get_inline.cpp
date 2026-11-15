// A read's inline cache test: the site's first two ways (an install fills way
// 0 and moves the others down) are compared with the receiver's shape inline,
// one compare and branch each. The way that names the shape answers here when
// its entry is an own data slot (the slot), a negative entry while the
// prototype epoch holds (`undefined`), or a data property one prototype link
// up while the epoch holds (the holder's slot). Anything else, and every
// shape neither way names, is the stub, which reads the same words.
//
// Every read of an ahead-of-time build that has a site takes this form; which
// shapes a site meets is the runtime's knowledge, in the ways, and nothing
// about the site is decided at compile time.
//
// The miss calls the MOVE-TO-FRONT stub (kPropGetFrontStub): when the
// per-thread mega table answers a site whose ways are full, it installs the
// answer in way 0 and moves the others down, so a way still holding a shape
// that stopped arriving is retaken.
#include "il_abi.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <string>
#include <utility>
#include <vector>

namespace il2mir {

namespace {
constexpr int32_t kIcShapeOffset = BRONZE_ABI_IC_SHAPE_OFFSET;
constexpr int32_t kIcSlotWordOffset = BRONZE_ABI_IC_SLOT_OFFSET;
constexpr int32_t kIcEpochOffset = BRONZE_ABI_IC_EPOCH_OFFSET;
constexpr int32_t kIcEntrySize = static_cast<int32_t>(BRONZE_ABI_IC_ENTRY_SIZE);
constexpr int64_t kIcOwnDataLimit = int64_t{1} << 32;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr int32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
// The negative entry's word is the flag alone (InlineCache::fillAbsent).
constexpr int64_t kAbsentWord = int64_t{BRONZE_ABI_IC_DEPTH_ABSENT_FLAG} << 32;
constexpr int kInlineWays = 2;
static_assert(BRONZE_ABI_IC_WAYS >= kInlineWays, "the inline test reads the site's own ways");
}  // namespace

Value* PropertyLoweringHelper::lower_prop_get_inline(Builder& b, Value* obj, uint32_t key_index, const IcSite& site,
                                                    bool run_once, const MemoCarry* carry) {
    if (!site || !enable_inlined_fastpaths() || in_process() || !ic_stubs() || key_index == kNoKey ||
        run_once) {
        return nullptr;
    }
    const int ways = kInlineWays;
    BasicBlock* bb_current = b.current_block();
    Function* fn = bb_current->parent();
    const std::string prefix = "ic_inline_" + std::to_string(fn->next_block_id());
    // append_block moves the insertion point: every block first.
    std::vector<BasicBlock*> found;
    std::vector<BasicBlock*> next;
    for (int w = 0; w < ways; ++w) {
        found.push_back(b.append_block(prefix + "_way" + std::to_string(w)));
        if (w + 1 < ways) next.push_back(b.append_block(prefix + "_next" + std::to_string(w + 1)));
    }
    BasicBlock* bb_tail = b.append_block(prefix + "_hit");
    Value* word = b.add_block_param(bb_tail, Type::i64());
    // The answering way's offset in the site, whose epoch only a not-own
    // answer reads.
    Value* entry_off = b.add_block_param(bb_tail, Type::i64());
    BasicBlock* bb_own = b.append_block(prefix + "_own");
    BasicBlock* bb_notown = b.append_block(prefix + "_notown");
    BasicBlock* bb_undef = b.append_block(prefix + "_undef");
    BasicBlock* bb_proto = b.append_block(prefix + "_proto");
    BasicBlock* bb_slow = b.append_block(prefix + "_miss_cold");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* merge_val = b.add_block_param(bb_merge, Type::i64());
    bb_slow->set_cold();
    std::vector<std::pair<BasicBlock*, bool>> edges;

    b.position_at_end(bb_current);
    // The keyed shape equals a way's shape exactly when the receiver is a
    // plain object of that shape; without the memo, a receiver that is no
    // plain object goes to the stub before its shape word is read.
    Value* site_at = site_ptr(b, site);
    Value* ptr = b.build_xor(obj, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    Value* shape = carry ? carry->keyed(b) : plain_shape_or_miss(b, obj, bb_slow);
    for (int w = 0; w < ways; ++w) {
        // Way 0's shape and slot words from its packed row (bronze_abi_layout.h,
        // "the HOT array"); its epoch, and every later way, from the site.
        const int32_t at = w * kIcEntrySize;
        Value* way_shape = w == 0 ? way0_shape(b, site) : b.build_load(Type::i64(), site_at, at + kIcShapeOffset);
        Value* match = b.build_eq(shape, way_shape);
        b.build_br_if(match, found[w], w + 1 < ways ? next[w] : bb_slow);
        b.position_at_end(found[w]);
        Value* way_word = w == 0 ? way0_slot_word(b, site) : b.build_load(Type::i64(), site_at, at + kIcSlotWordOffset);
        b.build_br(bb_tail, {way_word, b.build_iconst_i64(at)});
        if (w + 1 < ways) b.position_at_end(next[w]);
    }

    // The way that named the shape: its slot word decides the answer.
    b.position_at_end(bb_tail);
    b.build_br_if(b.build_ult(word, b.build_iconst_i64(kIcOwnDataLimit)), bb_own, bb_notown);

    b.position_at_end(bb_own);
    edges.emplace_back(bb_own, true);
    Value* own_val = load_slot_br(b, ptr, word);
    edges.back().first = b.current_block();
    b.build_br(bb_merge, {own_val});

    // Past the receiver's own slots an entry speaks only for the prototype
    // epoch it was filled at.
    b.position_at_end(bb_notown);
    Value* epoch = b.build_load(Type::i64(), b.build_pinned_tls_read(), BRONZE_TLS_PROTO_EPOCH_OFF);
    Value* current = b.build_eq(b.build_load(Type::i64(), b.build_add(site_at, entry_off), kIcEpochOffset), epoch);
    Value* absent = b.build_and(current, b.build_eq(word, b.build_iconst_i64(kAbsentWord)));
    b.build_br_if(absent, bb_undef, bb_proto);

    b.position_at_end(bb_undef);
    edges.emplace_back(bb_undef, true);
    b.build_br(bb_merge, {b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag))});

    // A data entry one link up: the depth half exactly 1, no flag (not an
    // accessor, not absent, not an f64 slot), at the current epoch. The
    // holder is reached the way the stub's walk reaches it (shape -> root
    // -> prototype): a plain object whose shape is no dictionary.
    BasicBlock* bb_root = b.append_block(prefix + "_root");
    BasicBlock* bb_holder = b.append_block(prefix + "_holder");
    BasicBlock* bb_holder_shape = b.append_block(prefix + "_hplain");
    BasicBlock* bb_holder_dict = b.append_block(prefix + "_hshape");
    BasicBlock* bb_load = b.append_block(prefix + "_pload");
    b.position_at_end(bb_proto);
    Value* depth_one = b.build_eq(b.build_lshr(word, b.build_iconst_i64(32)), b.build_iconst_i64(1));
    b.build_br_if(b.build_and(current, depth_one), bb_root, bb_slow);
    b.position_at_end(bb_root);
    Value* root = b.build_load(Type::i64(), shape, BRONZE_ABI_SHAPE_ROOT_OFFSET);
    b.build_br_if(b.build_ne(root, b.build_iconst_i64(0)), bb_holder, bb_slow);
    b.position_at_end(bb_holder);
    Value* pptr = b.build_xor(b.build_load(Type::i64(), root, BRONZE_ABI_SHAPE_PROTO_OFFSET),
                              b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(pptr, b.build_iconst_i64(kPayloadLimit)), bb_holder_shape, bb_slow);
    b.position_at_end(bb_holder_shape);
    Value* plain = b.build_eq(b.build_load(Type::i32(), pptr, 0), b.build_iconst_i32(kPlainHeaderLow));
    b.build_br_if(plain, bb_holder_dict, bb_slow);
    b.position_at_end(bb_holder_dict);
    Value* hshape = b.build_load(Type::i64(), pptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    Value* not_dict =
        b.build_eq(b.build_load(Type::i64(), hshape, BRONZE_ABI_SHAPE_DICT_OFFSET), b.build_iconst_i64(0));
    b.build_br_if(not_dict, bb_load, bb_slow);
    b.position_at_end(bb_load);
    Value* slot = b.build_and(word, b.build_iconst_i64(0xFFFFFFFFLL));
    edges.emplace_back(bb_load, true);
    b.build_br(bb_merge, {load_slot(b, pptr, slot)});

    b.position_at_end(bb_slow);
    // The stub that moves a table hit to way 0.
    Value* slow_val =
        b.build_call(kPropGetFrontStub, Type::i64(), {obj, runtime_key(b, key_index), site_at, hot_row_ptr(b, site)});
    edges.emplace_back(b.current_block(), false);
    b.build_br(bb_merge, {slow_val});

    if (carry) carry_memo(b, *carry, bb_merge, edges);
    b.position_at_end(bb_merge);
    return merge_val;
}

}  // namespace il2mir
