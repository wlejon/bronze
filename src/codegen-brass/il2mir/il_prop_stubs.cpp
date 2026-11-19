// The module's property stubs (il_prop_stubs.h): what a named read or write
// does when its inline way-0 test misses, before the runtime helper.
//
// A site's inline path answers way 0's own data slot. Everything else a site's
// words can say used to be the helper's: another way's shape, a prototype's
// slot, a getter, an absent key, the thread's megamorphic table, and the add
// a constructor's `this.x = x` makes on every `new` after the first. The
// helper re-derived each from the same words at the cost of a C++ entry,
// the key's lookup and the site scan; a method read through the prototype
// (`v.dot(...)` of a class instance) and every field a constructor adds were
// that path. These stubs read the words as the runtime's hit paths do
// (rt_prop.cpp propGetHelperBody, object.cpp ObjectHeader::setProp) and call
// the helper with the same operands for anything they do not answer, so the
// helper still owns every fill and every refusal.
#include "il_prop_stubs.h"

#include "il_abi.h"
#include "il_call_entry.h"
#include "il_property.h"

#include <brass/mir/block.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>

#include <string>
#include <vector>

namespace il2mir {

namespace {

constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr int32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int64_t kAccessorFlag = BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG;
constexpr int64_t kAbsentFlag = BRONZE_ABI_IC_DEPTH_ABSENT_FLAG;
// The deepest chain entry a stub walks; deeper ones are the helper's. Real
// class hierarchies sit well inside it.
constexpr int64_t kMaxWalk = 4;

Value* imm(Builder& b, int64_t v) { return b.build_iconst_i64(v); }

Value* tls(Builder& b) {
    Module* mod = b.current_block()->parent()->parent();
    return (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                               : b.build_call("bronze_tls_block_addr", Type::i64(), {});
}

// The word of a site's way `way` at `field`.
Value* way_word(Builder& b, Value* site, int way, int32_t field) {
    return b.build_load(Type::i64(), site, way * static_cast<int32_t>(BRONZE_ABI_IC_ENTRY_SIZE) + field);
}

// The thread's megamorphic entry for (`shape`, `key`) (bronze_abi_tls.h,
// mega_prop; the runtime's formula, rt_prop_mega.cpp): to `hit` with its slot
// word and epoch when it names that pair, else to `miss`. Only a FULL site
// asks (its last way holds a shape), as the helper does, so a site with room
// still warms to its own ways.
void mega_probe(Builder& b, Value* site, int last_way, Value* shape, Value* key, BasicBlock* hit, BasicBlock* miss) {
    BasicBlock* cur = b.current_block();
    BasicBlock* probe = b.append_block(std::string(cur->name()) + "_mega");
    b.position_at_end(cur);
    b.build_br_if(b.build_ne(way_word(b, site, last_way, BRONZE_ABI_IC_SHAPE_OFFSET), imm(b, 0)), probe, miss);
    b.position_at_end(probe);
    const auto mul = static_cast<int64_t>(BRONZE_ABI_MEGA_PROP_HASH_MUL);
    Value* hash = b.build_zext_i64(b.build_load(Type::i32(), shape, BRONZE_ABI_SHAPE_MEGA_HASH_OFFSET));
    Value* h = b.build_mul(b.build_xor(hash, b.build_mul(key, imm(b, mul))), imm(b, mul));
    Value* index = b.build_lshr(h, imm(b, 64 - BRONZE_ABI_MEGA_PROP_BITS));
    static_assert(BRONZE_ABI_MEGA_PROP_ENTRY_WORDS * 8 == 32, "an entry is 32 bytes");
    Value* at = b.build_add(tls(b), b.build_add(b.build_shl(index, imm(b, 5)), imm(b, BRONZE_TLS_MEGA_PROP_OFF)));
    Value* match = b.build_and(b.build_eq(b.build_load(Type::i64(), at, BRONZE_ABI_IC_SHAPE_OFFSET), shape),
                               b.build_eq(b.build_load(Type::i64(), at, BRONZE_ABI_MEGA_PROP_KEY_WORD * 8), key));
    b.build_br_if(match, hit,
                  {b.build_load(Type::i64(), at, BRONZE_ABI_IC_SLOT_OFFSET),
                   b.build_load(Type::i64(), at, BRONZE_ABI_IC_EPOCH_OFFSET)},
                  miss, {});
}

// The holder `links` (1..kMaxWalk) prototype links above an object at
// `shape`, walked as ObjectHeader::cachedProtoHolder walks (shape -> root ->
// prototype), each link a plain object whose shape is no dictionary or a
// dictionary marked a prototype (Shape::protoLinkCacheable), else
// `miss`. One straight copy per link: a loop's back edge would carry an
// interrupt poll. Branches to `found` with the holder's payload.
void walk_to_holder(Builder& b, Value* shape, Value* links, BasicBlock* found, BasicBlock* miss) {
    const std::string p = std::string(b.current_block()->name()) + "_walk";
    Value* cur_shape = shape;
    for (int64_t link = 1; link <= kMaxWalk; ++link) {
        const std::string at = p + std::to_string(link);
        BasicBlock* cur = b.current_block();
        BasicBlock* has_root = b.append_block(at + "_root");
        BasicBlock* is_obj = b.append_block(at + "_obj");
        BasicBlock* is_plain = b.append_block(at + "_plain");
        b.position_at_end(cur);
        Value* root = b.build_load(Type::i64(), cur_shape, BRONZE_ABI_SHAPE_ROOT_OFFSET);
        b.build_br_if(b.build_ne(root, imm(b, 0)), has_root, miss);
        b.position_at_end(has_root);
        Value* proto = b.build_load(Type::i64(), root, BRONZE_ABI_SHAPE_PROTO_OFFSET);
        Value* pptr = b.build_xor(proto, imm(b, static_cast<int64_t>(kObjectTagBits)));
        b.build_br_if(b.build_ult(pptr, imm(b, kPayloadLimit)), is_obj, miss);
        b.position_at_end(is_obj);
        b.build_br_if(b.build_eq(b.build_load(Type::i32(), pptr, 0), b.build_iconst_i32(kPlainHeaderLow)), is_plain,
                      miss);
        b.position_at_end(is_plain);
        Value* hshape = b.build_load(Type::i64(), pptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
        // Shape::protoLinkCacheable: no dictionary, or one marked a prototype
        // (every change to it that a read could see bumps the epoch).
        Value* not_dict = b.build_or(
            b.build_eq(b.build_load(Type::i64(), hshape, BRONZE_ABI_SHAPE_DICT_OFFSET), imm(b, 0)),
            b.build_ne(b.build_zext_i64(b.build_load(Type::i8(), hshape, BRONZE_ABI_SHAPE_USEDPROTO_OFFSET)), imm(b, 0)));
        if (link == kMaxWalk) {
            b.build_br_if(not_dict, found, {pptr}, miss, {});
            return;
        }
        BasicBlock* sound = b.append_block(at + "_sound");
        BasicBlock* deeper = b.append_block(at + "_deeper");
        b.position_at_end(is_plain);
        b.build_br_if(not_dict, sound, miss);
        b.position_at_end(sound);
        b.build_br_if(b.build_eq(links, imm(b, link)), found, {pptr}, deeper, {});
        b.position_at_end(deeper);
        cur_shape = hshape;
    }
}

}  // namespace

namespace {

// What a cache entry's words say about a plain receiver `ptr` (`obj` boxed)
// at `shape`, from the current block, which holds the entry's slot `word` and
// `epoch`: returns the read's value, or branches to `helper`.
//   own data (word < 2^32)          the slot
//   own accessor                    its getter called
//   chain entry, epoch current:
//     absent                        undefined
//     data, 1..kMaxWalk links       the holder's slot
//     accessor                      the holder's getter called
// The getter is entered as bronze_prop_get's hit enters it (rtEnterJs with no
// arguments), when the function says compiled code may enter it.
void answer_entry(Builder& b, Value* obj, Value* ptr, Value* shape, Value* word, Value* epoch, BasicBlock* helper,
                  const std::string& p) {
    BasicBlock* hit = b.current_block();
    BasicBlock* own = b.append_block(p + "_own");
    BasicBlock* flagged = b.append_block(p + "_flagged");
    BasicBlock* chain = b.append_block(p + "_chain");
    BasicBlock* epoch_ok = b.append_block(p + "_epoch_ok");
    BasicBlock* absent = b.append_block(p + "_absent");
    BasicBlock* not_absent = b.append_block(p + "_present");
    BasicBlock* walk = b.append_block(p + "_walk");
    BasicBlock* holder_bb = b.append_block(p + "_holder");
    Value* holder = b.add_block_param(holder_bb, Type::i64());
    BasicBlock* getter_bb = b.append_block(p + "_getter");
    Value* getter = b.add_block_param(getter_bb, Type::i64());

    b.position_at_end(hit);
    Value* depth = b.build_lshr(word, imm(b, 32));
    b.build_br_if(b.build_eq(depth, imm(b, 0)), own, flagged);

    b.position_at_end(own);
    b.build_ret(PropertyLoweringHelper::load_slot_at(b, ptr, word));

    // An own accessor (no links) needs no epoch: the receiver's shape says
    // the pair is there. Everything with links, and the absent answer, holds
    // only while the chain is the one the fill walked.
    b.position_at_end(flagged);
    Value* links = b.build_and(depth, imm(b, ~kAccessorFlag & 0xFFFFFFFFLL));
    b.build_br_if(b.build_eq(links, imm(b, 0)), holder_bb, {ptr}, chain, {});
    b.position_at_end(chain);
    b.build_br_if(b.build_eq(epoch, b.build_load(Type::i64(), tls(b), BRONZE_TLS_PROTO_EPOCH_OFF)), epoch_ok, helper);
    b.position_at_end(epoch_ok);
    b.build_br_if(b.build_eq(depth, imm(b, kAbsentFlag)), absent, not_absent);
    b.position_at_end(absent);
    b.build_ret(imm(b, static_cast<int64_t>(kUndefinedTag)));
    // Links 1..kMaxWalk with nothing but, at most, the accessor flag: any
    // other flag (statics) makes the count no small number.
    b.position_at_end(not_absent);
    b.build_br_if(b.build_ult(b.build_sub(links, imm(b, 1)), imm(b, kMaxWalk)), walk, helper);
    b.position_at_end(walk);
    walk_to_holder(b, shape, links, holder_bb, helper);

    b.position_at_end(holder_bb);
    Value* value = PropertyLoweringHelper::load_slot_at(b, holder, b.build_and(word, imm(b, 0xFFFFFFFFLL)));
    BasicBlock* value_bb = b.current_block();
    BasicBlock* data = b.append_block(p + "_data");
    b.position_at_end(value_bb);
    b.build_br_if(b.build_ne(b.build_and(depth, imm(b, kAccessorFlag)), imm(b, 0)), getter_bb, {value}, data, {});
    b.position_at_end(data);
    b.build_ret(value);

    b.position_at_end(getter_bb);
    Value* gfn = direct_callee(b, getter, b.build_iconst_i32(0), helper);
    b.build_ret(call_function_code(b, gfn, obj, b.build_iconst_i32(0), imm(b, 0)));
}

}  // namespace

// The read stub: (obj, key, site) -> value. Ways 0..3, then the megamorphic
// table; a way naming the receiver's shape is answered by answer_entry.
void build_prop_get_stub(Function* fn) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    Value* obj = b.add_block_param(entry, Type::i64());
    Value* key = b.add_block_param(entry, Type::i32());
    Value* site = b.add_block_param(entry, Type::ptr());
    BasicBlock* object = b.append_block("pg_object");
    BasicBlock* hit = b.append_block("pg_hit");
    Value* word = b.add_block_param(hit, Type::i64());
    Value* epoch = b.add_block_param(hit, Type::i64());
    BasicBlock* helper = b.append_block("pg_helper");

    b.position_at_end(entry);
    Value* ptr = b.build_xor(obj, imm(b, static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(ptr, imm(b, kPayloadLimit)), object, helper);

    b.position_at_end(object);
    Value* shape = b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    for (int way = 0; way < BRONZE_ABI_IC_WAYS; ++way) {
        BasicBlock* cur = b.current_block();
        BasicBlock* next = b.append_block("pg_way" + std::to_string(way + 1));
        b.position_at_end(cur);
        b.build_br_if(b.build_eq(shape, way_word(b, site, way, BRONZE_ABI_IC_SHAPE_OFFSET)), hit,
                      {way_word(b, site, way, BRONZE_ABI_IC_SLOT_OFFSET), way_word(b, site, way, BRONZE_ABI_IC_EPOCH_OFFSET)},
                      next, {});
        b.position_at_end(next);
    }
    mega_probe(b, site, BRONZE_ABI_IC_WAYS - 1, shape, b.build_zext_i64(key), hit, helper);

    b.position_at_end(hit);
    answer_entry(b, obj, ptr, shape, word, epoch, helper, "pg");

    b.position_at_end(helper);
    b.build_ret(b.build_call("bronze_prop_get", Type::i64(), {obj, key, site}));
    fn->rebuild_cfg_predecessors();
}

// The computed-read stub: (obj, key) -> value, what a computed read takes when
// its site has no cell or its cell has stopped taking fills: the thread's
// computed-read table (runtime/elem_ic.h), probed as elemCacheProbe probes it,
// for a plain receiver that is no dictionary and a hashed string, Number, or
// boolean key. String keys check first with an arena-interned/live identity
// latch (`key_ident`), so a hit compares shape + key_ident only; inline own
// slots bypass shape->dict and epoch loads. Everything else is bronze_elem_get.
void build_elem_get_stub(Function* fn) {
    static_assert(BRONZE_ABI_ELEM_ENTRY_SIZE == 64, "elem cache stride must stay a power of two");
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    Value* obj = b.add_block_param(entry, Type::i64());
    Value* key = b.add_block_param(entry, Type::i64());
    BasicBlock* object = b.append_block("eg_object");
    BasicBlock* plain = b.append_block("eg_plain");
    BasicBlock* str = b.append_block("eg_string");
    BasicBlock* str_tbl = b.append_block("eg_str_tbl");
    BasicBlock* str_probe = b.append_block("eg_str_probe");
    BasicBlock* str_beside = b.append_block("eg_str_beside");
    BasicBlock* not_str = b.append_block("eg_not_str");
    BasicBlock* not_num = b.append_block("eg_not_num");
    BasicBlock* prim_probe = b.append_block("eg_prim_probe");
    Value* pwitness = b.add_block_param(prim_probe, Type::i64());
    Value* pkind = b.add_block_param(prim_probe, Type::i64());
    BasicBlock* prim_tbl = b.append_block("eg_prim_tbl");
    BasicBlock* prim_beside = b.append_block("eg_prim_beside");
    BasicBlock* beside_hit = b.append_block("eg_beside_hit");
    Value* at_beside = b.add_block_param(beside_hit, Type::i64());
    BasicBlock* promote = b.append_block("eg_promote");
    BasicBlock* hit_fast = b.append_block("eg_hit_fast");
    Value* at = b.add_block_param(hit_fast, Type::i64());
    BasicBlock* hit_inline = b.append_block("eg_hit_inline");
    BasicBlock* hit_slow = b.append_block("eg_hit_slow");
    BasicBlock* hit_shaped = b.append_block("eg_hit_shaped");
    BasicBlock* hit_dict = b.append_block("eg_hit_dict");
    BasicBlock* dict_read = b.append_block("eg_dict_read");
    BasicBlock* helper = b.append_block("eg_helper");

    b.position_at_end(entry);
    Value* ptr = b.build_xor(obj, imm(b, static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(ptr, imm(b, kPayloadLimit)), object, helper);
    b.position_at_end(object);
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kPlainHeaderLow)), plain, helper);
    b.position_at_end(plain);
    Value* shape = b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);

    const auto entry_at = [&](Value* tbl, Value* index) {
        return b.build_add(tbl, b.build_shl(index, imm(b, 6)));
    };
    const auto bucket_of = [&](Value* witness) {
        Value* h = b.build_add(b.build_mul(shape, imm(b, static_cast<int64_t>(BRONZE_ABI_ELEM_HASH_MUL_SHAPE))),
                               b.build_mul(witness, imm(b, static_cast<int64_t>(BRONZE_ABI_ELEM_HASH_MUL_WITNESS))));
        return b.build_lshr(h, imm(b, BRONZE_ABI_ELEM_BUCKET_SHIFT));
    };

    // String keys first: the dominant case for computed property reads.
    Value* sptr = b.build_xor(key, imm(b, static_cast<int64_t>(uint64_t{BRONZE_ABI_TAG_STRING} << 48)));
    b.build_br_if(b.build_ult(sptr, imm(b, kPayloadLimit)), str, not_str);

    b.position_at_end(str);
    Value* sflags = b.build_zext_i64(b.build_load(Type::i32(), sptr, BRONZE_ABI_STRING_FLAGS_OFFSET));
    b.build_br_if(b.build_ne(b.build_and(sflags, imm(b, BRONZE_ABI_STRING_HASHED_BIT)), imm(b, 0)), str_tbl, helper);

    b.position_at_end(str_tbl);
    Value* stbl = b.build_load(Type::i64(), tls(b), BRONZE_TLS_ELEM_CACHE_TBL_OFF);
    b.build_br_if(b.build_ne(stbl, imm(b, 0)), str_probe, helper);

    b.position_at_end(str_probe);
    Value* switness = b.build_and(sflags, imm(b, BRONZE_ABI_STRING_HASH_MASK));
    Value* sbucket = bucket_of(switness);
    const auto str_matches = [&](Value* e) {
        return b.build_and(
            b.build_eq(b.build_load(Type::i64(), e, BRONZE_ABI_ELEM_IC_OFFSET + BRONZE_ABI_IC_SHAPE_OFFSET), shape),
            b.build_eq(b.build_load(Type::i64(), e, BRONZE_ABI_ELEM_IDENT_OFFSET), key));
    };
    Value* sfirst = entry_at(stbl, sbucket);
    b.build_br_if(str_matches(sfirst), hit_fast, {sfirst}, str_beside, {});

    b.position_at_end(str_beside);
    Value* ssecond = entry_at(stbl, b.build_xor(sbucket, imm(b, 1)));
    b.build_br_if(str_matches(ssecond), beside_hit, {ssecond}, helper, {});

    // Number or boolean keys.
    b.position_at_end(not_str);
    b.build_br_if(b.build_ule(key, imm(b, static_cast<int64_t>(kBronzeNumberMaxBits))), prim_probe,
                  {key, imm(b, BRONZE_ABI_ELEM_KIND_NUMBER)}, not_num, {});
    b.position_at_end(not_num);
    Value* bool_bit = b.build_xor(key, imm(b, static_cast<int64_t>(BRONZE_ABI_FALSE_BITS)));
    b.build_br_if(b.build_ult(bool_bit, imm(b, 2)), prim_probe,
                  {bool_bit, imm(b, BRONZE_ABI_ELEM_KIND_BOOL)}, helper, {});

    b.position_at_end(prim_probe);
    Value* ptbl = b.build_load(Type::i64(), tls(b), BRONZE_TLS_ELEM_CACHE_TBL_OFF);
    b.build_br_if(b.build_ne(ptbl, imm(b, 0)), prim_tbl, helper);

    b.position_at_end(prim_tbl);
    Value* pbucket = bucket_of(pwitness);
    const auto prim_matches = [&](Value* e) {
        Value* m = b.build_and(
            b.build_eq(b.build_load(Type::i64(), e, BRONZE_ABI_ELEM_IC_OFFSET + BRONZE_ABI_IC_SHAPE_OFFSET), shape),
            b.build_eq(b.build_load(Type::i64(), e, BRONZE_ABI_ELEM_WITNESS_OFFSET), pwitness));
        return b.build_and(
            m, b.build_eq(b.build_zext_i64(b.build_load(Type::i8(), e, BRONZE_ABI_ELEM_KIND_OFFSET)), pkind));
    };
    Value* pfirst = entry_at(ptbl, pbucket);
    b.build_br_if(prim_matches(pfirst), hit_fast, {pfirst}, prim_beside, {});

    b.position_at_end(prim_beside);
    Value* psecond = entry_at(ptbl, b.build_xor(pbucket, imm(b, 1)));
    b.build_br_if(prim_matches(psecond), beside_hit, {psecond}, helper, {});

    // Counted, so that a pair read this often from beside its bucket is moved
    // into it by the runtime's probe (BRONZE_ABI_ELEM_BESIDE_PROMOTE).
    b.position_at_end(beside_hit);
    Value* hits = b.build_load(Type::i32(), at_beside, BRONZE_ABI_ELEM_BESIDE_HITS_OFFSET);
    Value* bumped = b.build_add(hits, b.build_iconst_i32(1));
    b.build_store(Type::i32(), at_beside, BRONZE_ABI_ELEM_BESIDE_HITS_OFFSET, bumped);
    b.build_br_if(b.build_uge(bumped, b.build_iconst_i32(BRONZE_ABI_ELEM_BESIDE_PROMOTE)),
                  promote, {}, hit_fast, {at_beside});
    b.position_at_end(promote);
    b.build_br(helper);

    b.position_at_end(hit_fast);
    Value* word = b.build_load(Type::i64(), at, BRONZE_ABI_ELEM_IC_OFFSET + BRONZE_ABI_IC_SLOT_OFFSET);
    b.build_br_if(b.build_ult(word, imm(b, kBronzeObjOverflowSlot)), hit_inline, hit_slow);

    b.position_at_end(hit_inline);
    b.build_ret(b.build_load_indexed(Type::i64(), ptr, word, 8, kBronzeObjSlotsOffset));

    b.position_at_end(hit_slow);
    Value* epoch = b.build_load(Type::i64(), at, BRONZE_ABI_ELEM_IC_OFFSET + BRONZE_ABI_IC_EPOCH_OFFSET);
    Value* dict = b.build_load(Type::i64(), shape, BRONZE_ABI_SHAPE_DICT_OFFSET);
    b.build_br_if(b.build_eq(dict, imm(b, 0)), hit_shaped, hit_dict);
    b.position_at_end(hit_shaped);
    answer_entry(b, obj, ptr, shape, word, epoch, helper, "eg");
    // A dictionary's own data slot, while its table is at the entry's
    // generation (a frozen dictionary's shape-path entries go to the runtime,
    // as before).
    b.position_at_end(hit_dict);
    Value* dict_form = b.build_eq(b.build_lshr(word, imm(b, 32)), imm(b, BRONZE_ABI_ELEM_CELL_DICT));
    Value* current = b.build_eq(epoch, b.build_load(Type::i64(), dict, BRONZE_ABI_DICT_GENERATION_OFFSET));
    b.build_br_if(b.build_and(dict_form, current), dict_read, helper);
    b.position_at_end(dict_read);
    b.build_ret(PropertyLoweringHelper::load_slot_at(b, ptr, b.build_and(word, imm(b, 0xFFFFFFFFLL))));

    b.position_at_end(helper);
    b.build_ret(b.build_call("bronze_elem_get", Type::i64(), {obj, key}));
    fn->rebuild_cfg_predecessors();
}

// The write stub: (obj, key, val, site, mode) -> void, `mode` the write's
// (il_ast.h: 0 sloppy, 1 strict, kDefineWriteMode, kLiteralElementWriteMode).
//   ways 0..2, then the megamorphic table: a way naming the receiver's shape
//     own data slot                  the store, a value the slot's
//                                    representation takes (a double slot a
//                                    Number, an object slot a plain object)
//     accessor (an assignment)       its setter called
//   no way: way 0's add (ObjectHeader::setProp's transition hit), into an
//     inline slot of a plain object
// The barrier follows every store of a value that is no Number.
void build_prop_set_stub(Function* fn, const std::string& setter_call_sym) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    Value* obj = b.add_block_param(entry, Type::i64());
    Value* key = b.add_block_param(entry, Type::i32());
    Value* val = b.add_block_param(entry, Type::i64());
    Value* site = b.add_block_param(entry, Type::ptr());
    Value* mode = b.add_block_param(entry, Type::i32());
    BasicBlock* object = b.append_block("ps_object");
    BasicBlock* way_promote = b.append_block("ps_way_promote");
    Value* wp_word = b.add_block_param(way_promote, Type::i64());
    Value* wp_epoch = b.add_block_param(way_promote, Type::i64());
    BasicBlock* wp_count = b.append_block("ps_wp_count");
    BasicBlock* wp_take = b.append_block("ps_wp_take");
    BasicBlock* relatch_count = b.append_block("ps_relatch_count");
    Value* rc_word = b.add_block_param(relatch_count, Type::i64());
    Value* rc_epoch = b.add_block_param(relatch_count, Type::i64());
    BasicBlock* relatch_take = b.append_block("ps_relatch_take");
    BasicBlock* own = b.append_block("ps_own");
    Value* word = b.add_block_param(own, Type::i64());
    Value* epoch = b.add_block_param(own, Type::i64());
    BasicBlock* store = b.append_block("ps_store");
    Value* slot = b.add_block_param(store, Type::i64());
    BasicBlock* add = b.append_block("ps_add");
    BasicBlock* barrier = b.append_block("ps_barrier");
    Value* slot_addr = b.add_block_param(barrier, Type::i64());
    BasicBlock* done = b.append_block("ps_done");
    BasicBlock* helper = b.append_block("ps_helper");

    b.position_at_end(entry);
    Value* ptr = b.build_xor(obj, imm(b, static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(ptr, imm(b, kPayloadLimit)), object, helper);

    b.position_at_end(object);
    Value* shape = b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    for (int way = 0; way < BRONZE_ABI_IC_SET_WAYS; ++way) {
        BasicBlock* cur = b.current_block();
        BasicBlock* next = b.append_block("ps_way" + std::to_string(way + 1));
        b.position_at_end(cur);
        b.build_br_if(b.build_eq(shape, way_word(b, site, way, BRONZE_ABI_IC_SHAPE_OFFSET)),
                      way == 0 ? own : way_promote,
                      {way_word(b, site, way, BRONZE_ABI_IC_SLOT_OFFSET), way_word(b, site, way, BRONZE_ABI_IC_EPOCH_OFFSET)},
                      next, {});
        b.position_at_end(next);
    }
    // The megamorphic table answers a site up to BRONZE_ABI_IC_SET_MEGA_RELATCH
    // times, counted in word 11 (BRONZE_ABI_IC_SET_MEGA_COUNT_WORD); then one
    // miss goes to the helper, which zeroes the counter and swaps the active
    // shape into way 0 (rt_prop_write.cpp). Warm secondary-way hits count
    // separately in word 10 (BRONZE_ABI_IC_SET_PROMOTE_WORD) so 2/3-shape sites
    // never set word 11's ic_harvest megamorphic marker. A shape the table does
    // not name either is the add's to test.
    Value* set_key = b.build_or(b.build_zext_i64(key), imm(b, BRONZE_ABI_MEGA_PROP_SET_KEY_BIT));
    mega_probe(b, site, BRONZE_ABI_IC_SET_WAYS - 1, shape, set_key, relatch_count, add);

    b.position_at_end(way_promote);
    {
        Value* is_assign = b.build_ult(mode, b.build_iconst_i32(static_cast<int32_t>(kDefineWriteMode)));
        Value* hi = b.build_lshr(wp_word, imm(b, 32));
        Value* inlineable = b.build_or(b.build_eq(hi, imm(b, 0)),
                                       b.build_eq(hi, imm(b, BRONZE_ABI_IC_DEPTH_DOUBLE_FLAG)));
        b.build_br_if(b.build_and(is_assign, inlineable), wp_count, {}, own, {wp_word, wp_epoch});
    }
    b.position_at_end(wp_count);
    {
        constexpr int32_t kPromoteOff = BRONZE_ABI_IC_SET_PROMOTE_WORD * 8;
        Value* count = b.build_load(Type::i64(), site, kPromoteOff);
        b.build_br_if(b.build_ult(count, imm(b, BRONZE_ABI_IC_SET_MEGA_RELATCH)), wp_take, {}, helper, {});
        b.position_at_end(wp_take);
        b.build_store(Type::i64(), site, kPromoteOff, b.build_add(count, imm(b, 1)));
        b.build_br(own, {wp_word, wp_epoch});
    }

    b.position_at_end(relatch_count);
    {
        constexpr int32_t kCountOff = BRONZE_ABI_IC_SET_MEGA_COUNT_WORD * 8;
        Value* count = b.build_load(Type::i64(), site, kCountOff);
        b.build_br_if(b.build_ult(count, imm(b, BRONZE_ABI_IC_SET_MEGA_RELATCH)), relatch_take, {}, helper, {});
        b.position_at_end(relatch_take);
        b.build_store(Type::i64(), site, kCountOff, b.build_add(count, imm(b, 1)));
        b.build_br(own, {rc_word, rc_epoch});
    }

    // The value a slot of each representation takes as it is: a Number's bits
    // are a double slot's; an object slot takes a plain object.
    auto is_number = [&](Builder& bb) { return bb.build_ule(val, imm(bb, static_cast<int64_t>(kBronzeNumberMaxBits))); };
    // To `yes` when `val` is a plain object, else `no`.
    auto plain_object_test = [&](BasicBlock* yes, const std::vector<Value*>& yes_args, BasicBlock* no) {
        BasicBlock* cur = b.current_block();
        BasicBlock* hdr = b.append_block("ps_valobj");
        b.position_at_end(cur);
        Value* vptr = b.build_xor(val, imm(b, static_cast<int64_t>(kObjectTagBits)));
        b.build_br_if(b.build_ult(vptr, imm(b, kPayloadLimit)), hdr, no);
        b.position_at_end(hdr);
        b.build_br_if(b.build_eq(b.build_load(Type::i32(), vptr, 0), b.build_iconst_i32(kPlainHeaderLow)), yes,
                      Span<Value* const>(yes_args.data(), yes_args.size()), no, {});
    };

    b.position_at_end(own);
    {
        BasicBlock* flagged = b.append_block("ps_flagged");
        BasicBlock* dbl = b.append_block("ps_double");
        BasicBlock* not_dbl = b.append_block("ps_not_double");
        BasicBlock* objslot = b.append_block("ps_objslot");
        BasicBlock* accessor = b.append_block("ps_accessor");
        BasicBlock* chain = b.append_block("ps_chain");
        BasicBlock* walk = b.append_block("ps_walk");
        BasicBlock* holder_bb = b.append_block("ps_holder");
        Value* holder = b.add_block_param(holder_bb, Type::i64());
        b.position_at_end(own);
        b.build_br_if(b.build_ult(word, imm(b, int64_t{1} << 32)), store, {word}, flagged, {});
        b.position_at_end(flagged);
        Value* hi = b.build_lshr(word, imm(b, 32));
        Value* lo = b.build_and(word, imm(b, 0xFFFFFFFFLL));
        b.build_br_if(b.build_eq(hi, imm(b, BRONZE_ABI_IC_DEPTH_DOUBLE_FLAG)), dbl, not_dbl);
        b.position_at_end(dbl);
        b.build_br_if(is_number(b), store, {lo}, helper, {});
        b.position_at_end(not_dbl);
        b.build_br_if(b.build_eq(hi, imm(b, BRONZE_ABI_IC_DEPTH_OBJECT_FLAG)), objslot, accessor);
        b.position_at_end(objslot);
        plain_object_test(store, {lo}, helper);

        // An accessor: its setter is the slot after the getter. A
        // definition never runs one (the helper's question).
        b.position_at_end(accessor);
        Value* links = b.build_and(hi, imm(b, ~kAccessorFlag & 0xFFFFFFFFLL));
        Value* is_acc = b.build_ne(b.build_and(hi, imm(b, kAccessorFlag)), imm(b, 0));
        Value* assign = b.build_ult(mode, b.build_iconst_i32(static_cast<int32_t>(kDefineWriteMode)));
        BasicBlock* acc_ok = b.append_block("ps_acc_ok");
        b.position_at_end(accessor);
        b.build_br_if(b.build_and(is_acc, assign), acc_ok, helper);
        b.position_at_end(acc_ok);
        b.build_br_if(b.build_eq(links, imm(b, 0)), holder_bb, {ptr}, chain, {});
        b.position_at_end(chain);
        Value* current = b.build_eq(epoch, b.build_load(Type::i64(), tls(b), BRONZE_TLS_PROTO_EPOCH_OFF));
        b.build_br_if(b.build_and(current, b.build_ult(b.build_sub(links, imm(b, 1)), imm(b, kMaxWalk))), walk, helper);
        b.position_at_end(walk);
        walk_to_holder(b, shape, links, holder_bb, helper);
        b.position_at_end(holder_bb);
        Value* setter = PropertyLoweringHelper::load_slot_at(b, holder, b.build_add(lo, imm(b, 1)));
        Value* sfn = direct_callee(b, setter, b.build_iconst_i32(1), helper);
        b.build_call(setter_call_sym, Type::void_type(), {sfn, obj, val});
        b.build_ret_void();
    }

    b.position_at_end(store);
    {
        Value* at = PropertyLoweringHelper::slot_address(b, ptr, slot);
        b.build_store(Type::i64(), at, 0, val);
        b.build_br_if(b.build_ugt(val, imm(b, static_cast<int64_t>(kBronzeNumberMaxBits))), barrier, {at}, done, {});
    }

    // The add, way 0's: each test is one of the runtime transition hit's
    // conditions (object.cpp, ObjectHeader::setProp). The way's shape is the
    // one after the add; the receiver is a plain object at its parent, no
    // dictionary and no prototype (an add to a prototype bumps the epoch,
    // the helper's job), with the header that shape's objects have; the new
    // node is a plain enumerable/writable/configurable data property, whose
    // representation never generalized and takes the value; the epoch says
    // no setter appeared on the chain since the fill walked it; the way's
    // slot is the node's; and the slot is inline.
    b.position_at_end(add);
    {
        auto require = [&](Value* ok) {
            BasicBlock* cur = b.current_block();
            BasicBlock* next = b.append_block("ps_add_t");
            b.position_at_end(cur);
            b.build_br_if(ok, next, helper);
            b.position_at_end(next);
        };
        Value* cached = way_word(b, site, 0, BRONZE_ABI_IC_SHAPE_OFFSET);
        require(b.build_ugt(cached, imm(b, static_cast<int64_t>(BRONZE_ABI_IC_SHAPE_ARRAY_METHOD))));
        require(b.build_eq(b.build_load(Type::i64(), cached, BRONZE_ABI_SHAPE_PARENT_OFFSET), shape));
        Value* header = b.build_load(Type::i64(), ptr, 0);
        require(b.build_eq(b.build_trunc_i32(header), b.build_iconst_i32(kPlainHeaderLow)));
        require(b.build_eq(b.build_load(Type::i64(), cached, BRONZE_ABI_SHAPE_BARE_HEADER_OFFSET), header));
        require(b.build_eq(b.build_load(Type::i64(), shape, BRONZE_ABI_SHAPE_DICT_OFFSET), imm(b, 0)));
        require(b.build_eq(b.build_zext_i64(b.build_load(Type::i8(), shape, BRONZE_ABI_SHAPE_USEDPROTO_OFFSET)),
                           imm(b, 0)));
        require(b.build_eq(b.build_load(Type::i32(), cached, BRONZE_ABI_SHAPE_ATTRS_OFFSET),
                           b.build_iconst_i32(static_cast<int32_t>(BRONZE_ABI_SHAPE_ATTRS_PLAIN_DATA))));
        require(b.build_eq(b.build_zext_i64(b.build_load(Type::i8(), cached, BRONZE_ABI_SHAPE_REPR_GENERALIZED_OFFSET)),
                           imm(b, 0)));
        require(b.build_eq(way_word(b, site, 0, BRONZE_ABI_IC_EPOCH_OFFSET),
                           b.build_load(Type::i64(), tls(b), BRONZE_TLS_PROTO_EPOCH_OFF)));
        Value* new_slot = b.build_zext_i64(b.build_load(Type::i32(), cached, BRONZE_ABI_SHAPE_SLOTINDEX_OFFSET));
        require(b.build_eq(b.build_and(way_word(b, site, 0, BRONZE_ABI_IC_SLOT_OFFSET), imm(b, 0xFFFFFFFFLL)), new_slot));
        // An inline slot, or an overflow slot the object's block already has
        // room for (ObjectHeader::ensureOverflow sizes the block; growing it
        // allocates, the helper's job).
        {
            BasicBlock* cur = b.current_block();
            BasicBlock* out = b.append_block("ps_add_overflow");
            BasicBlock* room = b.append_block("ps_add_room");
            BasicBlock* ok = b.append_block("ps_add_slot_ok");
            b.position_at_end(cur);
            b.build_br_if(b.build_ult(new_slot, imm(b, kBronzeObjOverflowSlot)), ok, out);
            b.position_at_end(out);
            Value* block = b.build_xor(b.build_load(Type::i64(), ptr, kBronzeObjOverflowOffset),
                                       imm(b, static_cast<int64_t>(kObjectTagBits)));
            b.build_br_if(b.build_ult(block, imm(b, kPayloadLimit)), room, helper);
            b.position_at_end(room);
            // Slot 62 + i is Value i of the block, whose header (8 bytes)
            // counts its own size: Value i fits when 8 * (i + 2) <= size.
            Value* size = b.build_zext_i64(b.build_load(Type::i32(), block, BRONZE_ABI_HDR_SIZE_OFFSET));
            Value* need = b.build_shl(b.build_sub(new_slot, imm(b, kBronzeObjOverflowSlot - 2)), imm(b, 3));
            b.build_br_if(b.build_ule(need, size), ok, helper);
            b.position_at_end(ok);
        }
        BasicBlock* tested = b.current_block();
        BasicBlock* commit = b.append_block("ps_add_commit");
        BasicBlock* not_boxed = b.append_block("ps_add_repr");
        BasicBlock* not_double = b.append_block("ps_add_repr_obj");
        b.position_at_end(tested);
        Value* repr = b.build_zext_i64(b.build_load(Type::i8(), cached, BRONZE_ABI_SHAPE_REPR_OFFSET));
        b.build_br_if(b.build_eq(repr, imm(b, BRONZE_ABI_SLOT_REPR_BOXED)), commit, not_boxed);
        b.position_at_end(not_boxed);
        BasicBlock* dbl_ok = b.append_block("ps_add_double");
        b.position_at_end(not_boxed);
        b.build_br_if(b.build_eq(repr, imm(b, BRONZE_ABI_SLOT_REPR_DOUBLE)), dbl_ok, not_double);
        b.position_at_end(dbl_ok);
        b.build_br_if(is_number(b), commit, helper);
        b.position_at_end(not_double);
        BasicBlock* obj_ok = b.append_block("ps_add_object");
        b.position_at_end(not_double);
        b.build_br_if(b.build_eq(repr, imm(b, BRONZE_ABI_SLOT_REPR_OBJECT)), obj_ok, helper);
        b.position_at_end(obj_ok);
        plain_object_test(commit, {}, helper);

        b.position_at_end(commit);
        b.build_store(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET, cached);
        Value* at = PropertyLoweringHelper::slot_address(b, ptr, new_slot);
        b.build_store(Type::i64(), at, 0, val);
        b.build_br_if(b.build_ugt(val, imm(b, static_cast<int64_t>(kBronzeNumberMaxBits))), barrier, {at}, done, {});
    }

    b.position_at_end(barrier);
    b.build_write_barrier(slot_addr, val);
    b.build_br(done);
    b.position_at_end(done);
    b.build_ret_void();

    // A definition's miss is never the assignment's helper, whose chain walk
    // would run an inherited setter (il_ic_lowering.cpp lower_prop_set).
    b.position_at_end(helper);
    {
        BasicBlock* define = b.append_block("ps_define");
        BasicBlock* assign = b.append_block("ps_assign");
        b.position_at_end(helper);
        b.build_br_if(b.build_uge(mode, b.build_iconst_i32(static_cast<int32_t>(kDefineWriteMode))), define, assign);
        b.position_at_end(define);
        b.build_call("bronze_prop_define", Type::void_type(), {obj, key, val, site});
        b.build_ret_void();
        b.position_at_end(assign);
        b.build_call("bronze_prop_set", Type::void_type(),
                     {obj, key, val, site,
                      b.build_select(b.build_ne(mode, b.build_iconst_i32(0)), b.build_iconst_i32(1), b.build_iconst_i32(0))});
        b.build_ret_void();
    }
    fn->rebuild_cfg_predecessors();
}

// The cell stub: (obj, key, cell) -> value, what a computed read's inline
// cell test misses to (il_elem_lowering.cpp lower_elem_get). A plain
// receiver at the cell's shape with the cell's key bits whose slot word names
//   a dictionary's own slot: read while the receiver's table is at the
//     cell's generation;
//   a slot 1..BRONZE_ABI_ELEM_CELL_MAX_DEPTH links up: read off the holder
//     while the prototype epoch is the cell's, every link a plain object
//     that is no dictionary;
// everything else is bronze_elem_get_cell, which answers the read and fills
// the cell.
void build_elem_cell_stub(Function* fn) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    Value* obj = b.add_block_param(entry, Type::i64());
    Value* key = b.add_block_param(entry, Type::i64());
    Value* cell = b.add_block_param(entry, Type::i64());
    BasicBlock* object = b.append_block("ec_object");
    BasicBlock* hit = b.append_block("ec_hit");
    BasicBlock* dict = b.append_block("ec_dict");
    BasicBlock* dict_read = b.append_block("ec_dict_read");
    BasicBlock* proto = b.append_block("ec_proto");
    BasicBlock* walk = b.append_block("ec_walk");
    BasicBlock* holder_bb = b.append_block("ec_holder");
    Value* holder = b.add_block_param(holder_bb, Type::i64());
    BasicBlock* helper = b.append_block("ec_helper");
    auto word_at = [&](int32_t w) { return b.build_load(Type::i64(), cell, w * 8); };

    b.position_at_end(entry);
    Value* ptr = b.build_xor(obj, imm(b, static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(ptr, imm(b, kPayloadLimit)), object, helper);
    b.position_at_end(object);
    Value* shape = b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    b.build_br_if(b.build_and(b.build_eq(shape, word_at(0)), b.build_eq(key, word_at(1))), hit, helper);

    b.position_at_end(hit);
    Value* slot_word = word_at(2);
    Value* where = b.build_lshr(slot_word, imm(b, 32));
    Value* slot = b.build_and(slot_word, imm(b, 0xFFFFFFFFLL));
    b.build_br_if(b.build_eq(where, imm(b, BRONZE_ABI_ELEM_CELL_DICT)), dict, proto);

    b.position_at_end(dict);
    Value* table = b.build_load(Type::i64(), shape, BRONZE_ABI_SHAPE_DICT_OFFSET);
    b.build_br_if(b.build_eq(b.build_load(Type::i64(), table, BRONZE_ABI_DICT_GENERATION_OFFSET), word_at(4)), dict_read,
                  helper);
    b.position_at_end(dict_read);
    b.build_ret(PropertyLoweringHelper::load_slot_at(b, ptr, slot));

    b.position_at_end(proto);
    Value* linked = b.build_ult(b.build_sub(where, imm(b, 1)), imm(b, BRONZE_ABI_ELEM_CELL_MAX_DEPTH));
    Value* current = b.build_eq(word_at(4), b.build_load(Type::i64(), tls(b), BRONZE_TLS_PROTO_EPOCH_OFF));
    b.build_br_if(b.build_and(linked, current), walk, helper);
    b.position_at_end(walk);
    walk_to_holder(b, shape, where, holder_bb, helper);
    b.position_at_end(holder_bb);
    b.build_ret(PropertyLoweringHelper::load_slot_at(b, holder, slot));

    b.position_at_end(helper);
    b.build_ret(b.build_call("bronze_elem_get_cell", Type::i64(), {obj, key, cell}));
    fn->rebuild_cfg_predecessors();
}

// The setter call the write stub makes, in a function of its own: the
// argument block it roots would give the write stub a root frame to push and
// pop on every add and store it answers.
void build_setter_call_stub(Function* fn) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    Value* setter = b.add_block_param(entry, Type::i64());
    Value* this_val = b.add_block_param(entry, Type::i64());
    Value* val = b.add_block_param(entry, Type::i64());
    b.position_at_end(entry);
    Value* argv = b.build_alloca_tagged(1);
    b.build_store(Type::i64(), argv, 0, val);
    call_function_code(b, setter, this_val, b.build_iconst_i32(1), argv);
    b.build_ret_void();
    fn->rebuild_cfg_predecessors();
}

// The method stub: (recv, key, argc, argv, site) -> result, what a method
// site's inline way-0 arm misses to (il_call_entry.cpp,
// lower_method_call_inline). A plain receiver of
//   way 1's shape: its DIRECT entry (the same test as way 0's) or its SLOT
//     entry's callee;
//   no way's shape, at a full site (way 1 filled): the thread's
//     megamorphic table, which the helper's miss filled with the method's
//     read (rt_method_call.cpp): an own slot, or a data slot 1..kMaxWalk
//     links up while the epoch holds. Every BRONZE_ABI_METHOD_IC_MEGA_RELATCH-th
//     answer goes to the helper instead, whose latch moves the receiver into
//     way 0 (the count in word 11 is never reset; bronze_abi_layout.h).
// The callee is entered when the function says compiled code may enter it.
// Everything else is bronze_call_method with the same operands.
void build_method_call_stub(Function* fn) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    Value* recv = b.add_block_param(entry, Type::i64());
    Value* key = b.add_block_param(entry, Type::i32());
    Value* argc = b.add_block_param(entry, Type::i32());
    Value* argv = b.add_block_param(entry, Type::ptr());
    Value* site = b.add_block_param(entry, Type::ptr());
    BasicBlock* object = b.append_block("mc_object");
    BasicBlock* plain = b.append_block("mc_plain");
    BasicBlock* way1 = b.append_block("mc_way1");
    BasicBlock* direct = b.append_block("mc_direct");
    BasicBlock* way1_slot = b.append_block("mc_way1_slot");
    BasicBlock* gate = b.append_block("mc_mega_gate");
    BasicBlock* hit = b.append_block("mc_mega_hit");
    Value* word = b.add_block_param(hit, Type::i64());
    Value* epoch = b.add_block_param(hit, Type::i64());
    BasicBlock* chain = b.append_block("mc_chain");
    BasicBlock* holder_bb = b.append_block("mc_holder");
    Value* holder = b.add_block_param(holder_bb, Type::i64());
    BasicBlock* counted = b.append_block("mc_counted");
    Value* counted_fn = b.add_block_param(counted, Type::i64());
    BasicBlock* call = b.append_block("mc_call");
    Value* callee = b.add_block_param(call, Type::i64());
    BasicBlock* helper = b.append_block("mc_helper");
    auto word_at = [&](int32_t w) { return b.build_load(Type::i64(), site, w * 8); };

    BasicBlock* prim = b.append_block("mc_prim");
    BasicBlock* exotic = b.append_block("mc_exotic");
    b.position_at_end(entry);
    // Way 0's EXOTIC and PRIMITIVE forms (bronze_abi_layout.h, the method
    // site contract): word 0 odd, its kind field the receiver's heap kind or
    // value tag.
    Value* w0 = word_at(0);
    Value* w0_kind = b.build_and(b.build_lshr(w0, imm(b, BRONZE_ABI_METHOD_IC_KIND_SHIFT)),
                                 imm(b, 0xFFFFFFFFLL >> BRONZE_ABI_METHOD_IC_KIND_SHIFT));
    Value* w0_exotic = b.build_ne(b.build_and(w0, imm(b, static_cast<int64_t>(BRONZE_ABI_METHOD_IC_EXOTIC_BIT))), imm(b, 0));
    Value* ptr = b.build_xor(recv, imm(b, static_cast<int64_t>(kObjectTagBits)));
    b.build_br_if(b.build_ult(ptr, imm(b, kPayloadLimit)), object, prim);
    b.position_at_end(object);
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kPlainHeaderLow)), plain, exotic);

    // A string or number receiver: the SLOT form against the intrinsic in
    // the env word, whose shape the aux word pins; the slot's current
    // function is called (the `call` block's entry test).
    b.position_at_end(prim);
    {
        BasicBlock* holder_ok = b.append_block("mc_prim_holder");
        BasicBlock* holder_plain = b.append_block("mc_prim_plain");
        BasicBlock* read = b.append_block("mc_prim_read");
        b.position_at_end(prim);
        Value* is_number = b.build_ule(recv, imm(b, static_cast<int64_t>(kBronzeNumberMaxBits)));
        Value* kind = b.build_select(is_number, imm(b, BRONZE_ABI_METHOD_IC_KIND_NUMBER), b.build_lshr(recv, imm(b, 48)));
        Value* slot_plus_one = b.build_lshr(word_at(BRONZE_ABI_METHOD_IC_ARITY_WORD), imm(b, BRONZE_ABI_METHOD_IC_SLOT_SHIFT));
        Value* ok = b.build_and(b.build_and(w0_exotic, b.build_eq(w0_kind, kind)), b.build_ne(slot_plus_one, imm(b, 0)));
        b.build_br_if(ok, holder_ok, helper);
        b.position_at_end(holder_ok);
        Value* hptr = b.build_xor(word_at(BRONZE_ABI_METHOD_IC_ENV_WORD), imm(b, static_cast<int64_t>(kObjectTagBits)));
        b.build_br_if(b.build_ult(hptr, imm(b, kPayloadLimit)), holder_plain, helper);
        b.position_at_end(holder_plain);
        Value* plain_holder = b.build_eq(b.build_load(Type::i32(), hptr, 0), b.build_iconst_i32(kPlainHeaderLow));
        Value* same_shape = b.build_eq(b.build_load(Type::i64(), hptr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
                                       word_at(BRONZE_ABI_METHOD_IC_AUX_WORD));
        b.build_br_if(b.build_and(plain_holder, same_shape), read, helper);
        b.position_at_end(read);
        b.build_br(call, {PropertyLoweringHelper::load_slot_at(b, hptr, b.build_sub(slot_plus_one, imm(b, 1)))});
    }

    // An Array, a typed array or a RegExp: the kind against the receiver's
    // flags, the u64 at the carried offset against the aux word, the epoch
    // for every kind but an Array (whose bare shape and frozen prototype
    // need none), then the DIRECT words: a native or compiled builtin the
    // arguments cover. A function receiver's forms are the helper's.
    b.position_at_end(exotic);
    {
        BasicBlock* aux_test = b.append_block("mc_exotic_aux");
        BasicBlock* epoch_test = b.append_block("mc_exotic_epoch");
        BasicBlock* enter = b.append_block("mc_exotic_enter");
        b.position_at_end(exotic);
        static_assert(BRONZE_ABI_OBJ_FLAGS_OFFSET == 2, "the flags are the header word's bits 16..31");
        Value* flags = b.build_lshr(b.build_zext_i64(b.build_load(Type::i32(), ptr, 0)), imm(b, 16));
        Value* not_fn = b.build_ne(flags, imm(b, BRONZE_ABI_OBJ_FLAGS_FUNCTION));
        b.build_br_if(b.build_and(b.build_and(w0_exotic, b.build_eq(w0_kind, flags)), not_fn), aux_test, helper);
        b.position_at_end(aux_test);
        Value* aux = b.build_load(Type::i64(), b.build_add(ptr, b.build_lshr(w0, imm(b, BRONZE_ABI_METHOD_IC_BOX_SHIFT))), 0);
        Value* arity_word = word_at(BRONZE_ABI_METHOD_IC_ARITY_WORD);
        Value* entry_ok = b.build_ule(b.build_xor(arity_word, imm(b, static_cast<int64_t>(BRONZE_ABI_METHOD_IC_DIRECT_ENTRY_BIT))),
                                      b.build_zext_i64(argc));
        b.build_br_if(b.build_and(b.build_eq(aux, word_at(BRONZE_ABI_METHOD_IC_AUX_WORD)), entry_ok), epoch_test, helper);
        b.position_at_end(epoch_test);
        Value* epoch_ok = b.build_eq(word_at(BRONZE_ABI_METHOD_IC_EPOCH_WORD),
                                     b.build_load(Type::i64(), tls(b), BRONZE_TLS_PROTO_EPOCH_OFF));
        Value* array = b.build_eq(flags, imm(b, BRONZE_ABI_OBJ_FLAGS_ARRAY));
        b.build_br_if(b.build_or(array, epoch_ok), enter, helper);
        b.position_at_end(enter);
        b.build_ret(b.build_call_indirect(b.build_load(Type::ptr(), site, BRONZE_ABI_METHOD_IC_CODE_WORD * 8), Type::i64(),
                                          {word_at(BRONZE_ABI_METHOD_IC_ENV_WORD), recv, argc, argv}));
    }

    b.position_at_end(plain);
    Value* shape = b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    b.build_br_if(b.build_eq(shape, word_at(BRONZE_ABI_METHOD_IC_WAY1_SHAPE_WORD)), way1, gate);

    b.position_at_end(way1);
    Value* arity1 = word_at(BRONZE_ABI_METHOD_IC_WAY1_ARITY_WORD);
    Value* entry_ok = b.build_ule(b.build_xor(arity1, imm(b, static_cast<int64_t>(BRONZE_ABI_METHOD_IC_DIRECT_ENTRY_BIT))),
                                  b.build_zext_i64(argc));
    Value* epoch_ok = b.build_eq(word_at(BRONZE_ABI_METHOD_IC_WAY1_EPOCH_WORD),
                                 b.build_load(Type::i64(), tls(b), BRONZE_TLS_PROTO_EPOCH_OFF));
    b.build_br_if(b.build_and(entry_ok, epoch_ok), direct, way1_slot);
    b.position_at_end(direct);
    b.build_ret(b.build_call_indirect(
        b.build_load(Type::ptr(), site, BRONZE_ABI_METHOD_IC_WAY1_CODE_WORD * 8), Type::i64(),
        {word_at(BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD), recv, argc, argv}));
    b.position_at_end(way1_slot);
    {
        BasicBlock* slot_read = b.append_block("mc_way1_read");
        b.position_at_end(way1_slot);
        b.build_br_if(b.build_uge(arity1, imm(b, int64_t{1} << BRONZE_ABI_METHOD_IC_SLOT_SHIFT)), slot_read, helper);
        b.position_at_end(slot_read);
        Value* slot_word = b.build_sub(b.build_lshr(arity1, imm(b, BRONZE_ABI_METHOD_IC_SLOT_SHIFT)), imm(b, 1));
        b.build_br(call, {PropertyLoweringHelper::load_slot_at(b, ptr, slot_word)});
    }

    // A full site: way 1 holds a shape (a latch fills way 0 and moves the
    // resident to way 1).
    b.position_at_end(gate);
    static_assert(BRONZE_ABI_METHOD_IC_WAY1_SHAPE_WORD * 8 == 2 * BRONZE_ABI_IC_ENTRY_SIZE,
                  "way 1's shape word is where a property site's way 2 shape is");
    mega_probe(b, site, 2, shape, b.build_zext_i64(key), hit, helper);

    b.position_at_end(hit);
    {
        BasicBlock* own = b.append_block("mc_own");
        b.position_at_end(hit);
        Value* depth = b.build_lshr(word, imm(b, 32));
        b.build_br_if(b.build_eq(depth, imm(b, 0)), own, chain);
        b.position_at_end(own);
        b.build_br(counted, {PropertyLoweringHelper::load_slot_at(b, ptr, word)});
        b.position_at_end(chain);
        BasicBlock* walk = b.append_block("mc_walk");
        b.position_at_end(chain);
        Value* walkable = b.build_ult(b.build_sub(depth, imm(b, 1)), imm(b, kMaxWalk));
        Value* current = b.build_eq(epoch, b.build_load(Type::i64(), tls(b), BRONZE_TLS_PROTO_EPOCH_OFF));
        b.build_br_if(b.build_and(walkable, current), walk, helper);
        b.position_at_end(walk);
        walk_to_holder(b, shape, depth, holder_bb, helper);
    }
    b.position_at_end(holder_bb);
    b.build_br(counted, {PropertyLoweringHelper::load_slot_at(b, holder, b.build_and(word, imm(b, 0xFFFFFFFFLL)))});

    b.position_at_end(counted);
    {
        constexpr int32_t kCountOff = BRONZE_ABI_METHOD_IC_MEGA_COUNT_WORD * 8;
        Value* count = b.build_add(b.build_load(Type::i64(), site, kCountOff), imm(b, 1));
        b.build_store(Type::i64(), site, kCountOff, count);
        Value* relatch = b.build_eq(b.build_and(count, imm(b, BRONZE_ABI_METHOD_IC_MEGA_RELATCH - 1)), imm(b, 0));
        b.build_br_if(relatch, helper, {}, call, {counted_fn});
    }

    b.position_at_end(call);
    Value* fnp = direct_callee(b, callee, argc, helper);
    b.build_ret(call_function_code(b, fnp, recv, argc, argv));

    b.position_at_end(helper);
    b.build_ret(b.build_call("bronze_call_method", Type::i64(), {recv, key, argc, argv, site}));
    fn->rebuild_cfg_predecessors();
}

}  // namespace il2mir
