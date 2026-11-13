// The module's keyed-access stubs: what a computed read `o[k]` that would
// have called bronze_elem_get calls instead (and, below, the write's twin). It answers, from the computed-read
// cache the runtime fills (runtime/elem_ic.h, laid out in bronze_abi_layout.h
// "the COMPUTED-read cache"), the two hits that need nothing but loads — an
// own data slot of a plain receiver, and an absent key while the epoch holds —
// plus a data slot up the prototype chain while the epoch holds (the property
// stubs' walk, il_direct_common.h), and calls bronze_elem_get, with the same
// operands, for everything else.
//
// Why a stub and not the runtime's probe. A three.js frame makes millions of
// these reads (`this.map[name]` in WebGLUniforms, `enabledCapabilities[id]` in
// WebGLState, `cache[key]` in needsUpdate), all on plain receivers, and the
// runtime answered each through two calls into the shared library, a
// thread_local table address, the key's content compare against the entry's
// arena copy and the probe's result struct — about a hundred and fifty
// instructions for what is a hash, three compares and a load. The stub is one
// module-local call per read. A generic `o[k]` site (il_elem_lowering.cpp)
// also carries the own-slot hit inline, behind its array and view arms
// (build_inline_elem_get_hit below), because the call was most of what a hit
// cost; a run-once body calls the stub alone, since there the code size buys
// nothing.
//
// What the stub may trust, and why it is the runtime's own answer:
//  - the bucket is the runtime's `bucketOf`, constant for constant (the ABI's
//    ELEM_HASH words); a probe hashing differently would never hit, not answer
//    wrongly;
//  - a NUMBER key matches by its raw bits against the entry's witness (the
//    witness IS a number key's identity), and a STRING key by its bits against
//    the entry's identity latch, which the runtime keeps naming a live string
//    content-equal to the entry's key (bronze_abi_layout.h states the two
//    invariants); both also match the entry's kind, so a string entry's hash
//    can never pass for a number's bits;
//  - the entry's shape equal to the receiver's means the walk that filled it
//    found the key there and found the shape cacheable: a dictionary shape is
//    private to its object and minted fresh, so no shape changes kind under an
//    entry;
//  - a slot word below 2^32 is depth 0 with no flag (own data: no accessor, no
//    absence), which a shape match alone keeps valid (InlineCache::describes);
//    an ABSENT word additionally needs its fill epoch to be the thread's
//    (describesAbsent).

#include "il/il_pin.h"
#include "il_abi.h"
#include "il_direct_common.h"
#include "il_elem_stub.h"
#include "il_lowering.h"
#include "il_lowering_ops.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <string>

namespace il2mir {

namespace {

constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr uint64_t kStringTagBits = uint64_t{BRONZE_ABI_TAG_STRING} << 48;
constexpr uint64_t kTagMask = 0xFFFF000000000000ULL;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr int32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int64_t kOwnDataLimit = int64_t{1} << 32;
static_assert((kPlainHeaderLow & 1) == 1 && BRONZE_ABI_PROTO_EPOCH_FIRST % 2 == 0 && BRONZE_ABI_PROTO_EPOCH_STEP % 2 == 0,
              "the TLS block stands in for a non-object only while no epoch reads as a header");

Value* iconst(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

Value* tls_block(Builder& b) {
    Module* mod = b.current_block()->parent()->parent();
    return (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                               : b.build_call("bronze_tls_block_addr", Type::i64(), {});
}

// The entry of a computed-access table (`tbl_off` in the TLS block, its
// bucket the top bits past `shift`) that speaks for (obj's shape, key), and
// whether it does: `match` holds exactly when obj is a plain object and the
// entry's kind, key word and shape are this access's. Leaves the builder in
// the block that computes `match`; every refusal before it branches to `slow`.
struct KeyedEntry {
    Value* tls = nullptr;
    Value* ptr = nullptr;
    Value* entry = nullptr;
    Value* shape = nullptr;
    Value* match = nullptr;
    // What `entry_matches` asks of another entry of the table.
    Value* tbl = nullptr;
    Value* bucket = nullptr;
    Value* kind = nullptr;
    Value* match_off = nullptr;
};

// Whether `entry` speaks for the access `k` was probed for: its kind, its key
// word and its shape.
Value* entry_matches(Builder& b, const KeyedEntry& k, Value* key, Value* entry) {
    Value* kind_ok = b.build_eq(b.build_zext_i64(b.build_load(Type::i8(), entry, BRONZE_ABI_ELEM_KIND_OFFSET)), k.kind);
    Value* key_ok = b.build_eq(b.build_load(Type::i64(), b.build_add(entry, k.match_off), 0), key);
    Value* shape_ok = b.build_eq(
        b.build_load(Type::i64(), entry, BRONZE_ABI_ELEM_IC_OFFSET + BRONZE_ABI_IC_SHAPE_OFFSET), k.shape);
    return b.build_and(b.build_and(kind_ok, key_ok), shape_ok);
}

KeyedEntry keyed_entry(Builder& b, Value* obj, Value* key, int32_t tbl_off, int64_t shift, BasicBlock* slow,
                       const std::string& prefix) {
    // append_block moves the insertion point: the blocks first.
    BasicBlock* entry = b.current_block();
    BasicBlock* plain = b.append_block(prefix + "_plain");
    BasicBlock* number = b.append_block(prefix + "_number");
    BasicBlock* not_number = b.append_block(prefix + "_notnumber");
    BasicBlock* string = b.append_block(prefix + "_string");
    BasicBlock* hashed = b.append_block(prefix + "_hashed");
    BasicBlock* probe = b.append_block(prefix + "_probe");
    Value* witness = b.add_block_param(probe, Type::i64());
    Value* kind = b.add_block_param(probe, Type::i64());
    Value* match_off = b.add_block_param(probe, Type::i64());

    // A plain object, read through the TLS block for anything else (its first
    // word is the even epoch, which no header reads as), and a published
    // table: a thread that has not built its heap has none, and takes the
    // helper.
    b.position_at_end(entry);
    KeyedEntry k;
    k.tls = tls_block(b);
    Value* tbl = b.build_load(Type::i64(), k.tls, tbl_off);
    k.ptr = b.build_xor(obj, iconst(b, kObjectTagBits));
    Value* is_obj = b.build_ult(k.ptr, b.build_iconst_i64(kPayloadLimit));
    Value* base = b.build_select(is_obj, k.ptr, k.tls);
    Value* is_plain = b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kPlainHeaderLow));
    b.build_br_if(b.build_and(is_plain, b.build_ne(tbl, iconst(b, 0))), plain, slow);

    // The key's witness, and which entry word confirms it: a number's own bits
    // against the witness, a string's bits against the identity latch.
    b.position_at_end(plain);
    Value* shape = b.build_load(Type::i64(), k.ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    b.build_br_if(b.build_ule(key, iconst(b, kBronzeNumberMaxBits)), number, not_number);

    b.position_at_end(number);
    b.build_br(probe, {key, b.build_iconst_i64(BRONZE_ABI_ELEM_KIND_NUMBER),
                       b.build_iconst_i64(BRONZE_ABI_ELEM_WITNESS_OFFSET)});

    // A string whose hash is memoized; the helper's first probe memoizes it.
    b.position_at_end(not_number);
    b.build_br_if(b.build_eq(b.build_and(key, iconst(b, kTagMask)), iconst(b, kStringTagBits)), string, slow);
    b.position_at_end(string);
    Value* sptr = b.build_and(key, iconst(b, kPayloadMask));
    Value* flags = b.build_load(Type::i32(), sptr, BRONZE_ABI_STRING_FLAGS_OFFSET);
    b.build_br_if(b.build_ne(b.build_and(flags, b.build_iconst_i32(BRONZE_ABI_STRING_HASHED_BIT)),
                             b.build_iconst_i32(0)),
                  hashed, slow);
    b.position_at_end(hashed);
    Value* hash = b.build_zext_i64(
        b.build_and(flags, b.build_iconst_i32(static_cast<int32_t>(BRONZE_ABI_STRING_HASH_MASK))));
    b.build_br(probe, {hash, b.build_iconst_i64(BRONZE_ABI_ELEM_KIND_STRING),
                       b.build_iconst_i64(BRONZE_ABI_ELEM_IDENT_OFFSET)});

    // runtime/elem_ic.cpp's bucketOf(shape, witness), word for word.
    b.position_at_end(probe);
    Value* h = b.build_add(b.build_mul(shape, iconst(b, BRONZE_ABI_ELEM_HASH_MUL_SHAPE)),
                           b.build_mul(witness, iconst(b, BRONZE_ABI_ELEM_HASH_MUL_WITNESS)));
    Value* bucket = b.build_lshr(h, b.build_iconst_i64(shift));
    k.entry = b.build_add(tbl, b.build_mul(bucket, iconst(b, BRONZE_ABI_ELEM_ENTRY_SIZE)));
    k.shape = shape;
    k.tbl = tbl;
    k.bucket = bucket;
    k.kind = kind;
    k.match_off = match_off;
    k.match = entry_matches(b, k, key, k.entry);
    return k;
}

void build_elem_get_stub(PropertyLoweringHelper& pl, Function* fn) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* cur = b.append_block("entry");
    Value* obj = b.add_block_param(cur, Type::i64());
    Value* key = b.add_block_param(cur, Type::i64());
    BasicBlock* matched = b.append_block("eget_matched");
    BasicBlock* own = b.append_block("eget_own");
    BasicBlock* not_own = b.append_block("eget_notown");
    BasicBlock* absent = b.append_block("eget_absent");
    BasicBlock* not_absent = b.append_block("eget_notabsent");
    BasicBlock* chain = b.append_block("eget_chain");
    BasicBlock* slow = b.append_block("eget_slow");

    BasicBlock* second = b.append_block("eget_second");
    BasicBlock* beside_hit = b.append_block("eget_beside_hit");
    Value* entry = b.add_block_param(matched, Type::i64());

    b.position_at_end(cur);
    const KeyedEntry k = keyed_entry(b, obj, key, BRONZE_TLS_ELEM_CACHE_TBL_OFF, BRONZE_ABI_ELEM_BUCKET_SHIFT, slow,
                                     "eget");
    b.build_br_if(k.match, matched, {k.entry}, second, {});

    // The read table's second way (runtime/elem_ic.cpp, elemCacheProbe): the
    // entry beside the bucket, where a pair sits whose bucket another took.
    b.position_at_end(second);
    Value* beside = b.build_add(
        k.tbl, b.build_mul(b.build_xor(k.bucket, iconst(b, 1)), iconst(b, BRONZE_ABI_ELEM_ENTRY_SIZE)));
    b.build_br_if(entry_matches(b, k, key, beside), beside_hit, {}, slow, {});

    // Counted: a pair read this often from beside its bucket is handed to the
    // runtime once, whose probe gives it the bucket (elemCacheProbe), so the
    // site's own arm answers it from then on.
    b.position_at_end(beside_hit);
    Value* hits = b.build_add(b.build_load(Type::i32(), beside, BRONZE_ABI_ELEM_BESIDE_HITS_OFFSET),
                              b.build_iconst_i32(1));
    b.build_store(Type::i32(), beside, BRONZE_ABI_ELEM_BESIDE_HITS_OFFSET, hits);
    b.build_br_if(b.build_ult(hits, b.build_iconst_i32(BRONZE_ABI_ELEM_BESIDE_PROMOTE)), matched, {beside}, slow, {});

    b.position_at_end(matched);
    Value* slot_word = b.build_load(Type::i64(), entry, BRONZE_ABI_ELEM_IC_OFFSET + BRONZE_ABI_IC_SLOT_OFFSET);
    b.build_br_if(b.build_ult(slot_word, b.build_iconst_i64(kOwnDataLimit)), own, not_own);

    b.position_at_end(own);
    b.build_ret(pl.load_slot(b, k.ptr, slot_word));

    b.position_at_end(not_own);
    Value* depth = b.build_lshr(slot_word, iconst(b, 32));
    Value* is_absent = b.build_ne(b.build_and(depth, iconst(b, BRONZE_ABI_IC_DEPTH_ABSENT_FLAG)), iconst(b, 0));
    Value* epoch_ok =
        b.build_eq(b.build_load(Type::i64(), entry, BRONZE_ABI_ELEM_IC_OFFSET + BRONZE_ABI_IC_EPOCH_OFFSET),
                   b.build_load(Type::i64(), k.tls, BRONZE_TLS_PROTO_EPOCH_OFF));
    b.build_br_if(b.build_and(is_absent, epoch_ok), absent, not_absent);

    b.position_at_end(absent);
    b.build_ret(iconst(b, kUndefinedTag));

    // A data property up the chain while the epoch holds (`gl[name]`, a method
    // picked by a computed name off a class's prototype): the holder's slot,
    // each link asked what the runtime's probe asks of it.
    b.position_at_end(not_absent);
    Value* data = b.build_eq(b.build_and(depth, b.build_iconst_i64(direct::kAccessorDepth)), iconst(b, 0));
    b.build_br_if(b.build_and(epoch_ok, b.build_and(data, direct::walkable_depth(b, depth))), chain, slow);

    b.position_at_end(chain);
    Value* holder = direct::walk_to_prototype(b, k.shape, depth, slow, "eget");
    b.build_ret(pl.load_slot(b, holder, b.build_and(slot_word, b.build_iconst_i64(direct::kSlotMask))));

    b.position_at_end(slow);
    b.build_ret(b.build_call("bronze_elem_get", Type::i64(), {obj, key}));
    fn->rebuild_cfg_predecessors();
}

// The write stub: bronze_elem_set's operands. Its one hit is the runtime's
// computed-store hit (rt_prop_write.cpp, `describesOwn` on the write table's
// entry): an own, writable data slot of the receiver's shape, which only
// `setProp` fills. The slot word below 2^32 is depth 0 with no flag — no
// accessor, no absence, and no f64 slot, whose store must keep the slot's
// representation and is the helper's. Everything else is the helper.
void build_elem_set_stub(PropertyLoweringHelper& pl, Function* fn) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* cur = b.append_block("entry");
    Value* obj = b.add_block_param(cur, Type::i64());
    Value* key = b.add_block_param(cur, Type::i64());
    Value* val = b.add_block_param(cur, Type::i64());
    Value* strict = b.add_block_param(cur, Type::i32());
    BasicBlock* matched = b.append_block("eset_matched");
    BasicBlock* own = b.append_block("eset_own");
    BasicBlock* barrier = b.append_block("eset_barrier");
    BasicBlock* done = b.append_block("eset_done");
    BasicBlock* slow = b.append_block("eset_slow");

    b.position_at_end(cur);
    const KeyedEntry k = keyed_entry(b, obj, key, BRONZE_TLS_ELEM_SET_CACHE_TBL_OFF,
                                     BRONZE_ABI_ELEM_SET_BUCKET_SHIFT, slow, "eset");
    b.build_br_if(k.match, matched, slow);

    b.position_at_end(matched);
    Value* slot_word = b.build_load(Type::i64(), k.entry, BRONZE_ABI_ELEM_IC_OFFSET + BRONZE_ABI_IC_SLOT_OFFSET);
    b.build_br_if(b.build_ult(slot_word, b.build_iconst_i64(kOwnDataLimit)), own, slow);

    b.position_at_end(own);
    Value* slot_base = pl.slot_base(b, k.ptr, slot_word);
    Value* slot_addr = b.build_add(b.build_add(slot_base, b.build_shl(slot_word, iconst(b, 3))),
                                   b.build_iconst_i64(kBronzeObjSlotsOffset));
    b.build_store(Type::i64(), slot_addr, 0, val);
    b.build_br_if(b.build_ugt(val, iconst(b, kBronzeNumberMaxBits)), barrier, done);

    b.position_at_end(barrier);
    b.build_write_barrier(slot_addr, val);
    b.build_br(done);

    b.position_at_end(done);
    b.build_ret_void();

    b.position_at_end(slow);
    b.build_call("bronze_elem_set", Type::void_type(), {obj, key, val, strict});
    b.build_ret_void();
    fn->rebuild_cfg_predecessors();
}

// The constant-index stubs. `this._components[0] = r` and `m[12]` on a
// Float32Array are constant-index accesses, which lower as a dense Array's
// element with a call for any other receiver; the keyed stub's first question
// is whether the receiver is a plain object, and a typed array then went to
// the runtime's helper for every element (pixi's Color normalizes into a
// Float32Array four stores at a time, a few thousand a frame). Here a word
// view (Int32Array, Uint32Array, Float32Array, Float64Array) is asked first
// and answered as a site proven on one answers it (il_elem_word_view.cpp);
// the key is a Number by construction, and a store's value must be one.
// Everything else is the keyed stub, with the same operands.
void build_index_get_stub(PropertyLoweringHelper& pl, Function* fn) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* cur = b.append_block("entry");
    Value* obj = b.add_block_param(cur, Type::i64());
    Value* key = b.add_block_param(cur, Type::i64());
    BasicBlock* view = b.append_block("iget_view");
    BasicBlock* other = b.append_block("iget_other");

    b.position_at_end(cur);
    b.build_br_if(pl.lower_is_typed_array(b, obj, bronze::il::kTypedArrayWordView), view, other);

    b.position_at_end(view);
    b.build_ret(pl.lower_elem_get_word_view(b, obj, b.build_bitcast_f64_i64(key), 0));

    b.position_at_end(other);
    b.build_ret(b.build_call(kElemGetStub, Type::i64(), {obj, key}));
    fn->rebuild_cfg_predecessors();
}

void build_index_set_stub(PropertyLoweringHelper& pl, Function* fn) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* cur = b.append_block("entry");
    Value* obj = b.add_block_param(cur, Type::i64());
    Value* key = b.add_block_param(cur, Type::i64());
    Value* val = b.add_block_param(cur, Type::i64());
    Value* strict = b.add_block_param(cur, Type::i32());
    BasicBlock* view = b.append_block("iset_view");
    BasicBlock* other = b.append_block("iset_other");

    b.position_at_end(cur);
    Value* is_view = pl.lower_is_typed_array(b, obj, bronze::il::kTypedArrayWordView);
    b.build_br_if(b.build_and(is_view, b.build_ule(val, iconst(b, kBronzeNumberMaxBits))), view, other);

    // Key and value both Numbers, so the store has no helper arm and
    // `strict` no use.
    b.position_at_end(view);
    pl.lower_elem_set_word_view(b, obj, b.build_bitcast_f64_i64(key), pl.box_number(b, b.build_bitcast_f64_i64(val)), 0,
                                0);
    b.build_ret_void();

    b.position_at_end(other);
    b.build_call(kElemSetStub, Type::void_type(), {obj, key, val, strict});
    b.build_ret_void();
    fn->rebuild_cfg_predecessors();
}

}  // namespace

// The get stub's own-slot hit, at the site: the same probe, and on a hit the
// slot load straight into `merge`; everything else (an absent key, a miss, a
// receiver that is not a plain object) branches to `miss`, where the caller
// calls the stub, which answers the rest. Inline because the call was most of
// what a hit cost: three.js reads `map[name]` and `enabledCapabilities[id]`
// millions of times a frame, and the stub was the largest line item of those
// reads. The absent hit stays in the stub: inlining it too was more code at
// every site and measured no better (keyed microbench, 7.79G instructions
// against 7.72G).
void build_inline_elem_get_hit(PropertyLoweringHelper& pl, Builder& b, Value* obj, Value* key, BasicBlock* merge,
                               BasicBlock* miss) {
    // append_block moves the insertion point: the blocks first.
    BasicBlock* cur = b.current_block();
    const std::string prefix = "kget_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* matched = b.append_block(prefix + "_matched");
    BasicBlock* own = b.append_block(prefix + "_own");
    b.position_at_end(cur);
    const KeyedEntry k =
        keyed_entry(b, obj, key, BRONZE_TLS_ELEM_CACHE_TBL_OFF, BRONZE_ABI_ELEM_BUCKET_SHIFT, miss, prefix);
    b.build_br_if(k.match, matched, miss);

    b.position_at_end(matched);
    Value* slot_word = b.build_load(Type::i64(), k.entry, BRONZE_ABI_ELEM_IC_OFFSET + BRONZE_ABI_IC_SLOT_OFFSET);
    b.build_br_if(b.build_ult(slot_word, b.build_iconst_i64(kOwnDataLimit)), own, miss);

    b.position_at_end(own);
    b.build_br(merge, {pl.load_slot(b, k.ptr, slot_word)});
}

Value* boxed_elem_key(Builder& b, Value* index) {
    if (index->type() == Type::i32()) return b.build_bitcast_i64_f64(b.build_sitofp_f64_i32(index));
    if (index->type() == Type::f64()) return box_f64_bits(b, index);
    return index;
}

Value* PropertyLoweringHelper::lower_elem_get_call(Builder& b, Value* obj, Value* index) {
    return b.build_call(elem_get_helper(), Type::i64(), {obj, boxed_elem_key(b, index)});
}

void PropertyLoweringHelper::lower_elem_set_call(Builder& b, Value* obj, Value* index, Value* val,
                                                 uint32_t strict) {
    b.build_call(elem_set_helper(), Type::void_type(),
                 {obj, boxed_elem_key(b, index), val, b.build_iconst_i32(static_cast<int32_t>(strict))});
}

// What a computed access calls when its inline arms miss: the module's keyed
// stub once emit_elem_stubs has built one, else the runtime helper itself.
const char* PropertyLoweringHelper::elem_get_helper() const noexcept {
    return elem_stubs_ ? kElemGetStub : "bronze_elem_get";
}

const char* PropertyLoweringHelper::elem_set_helper() const noexcept {
    return elem_stubs_ ? kElemSetStub : "bronze_elem_set";
}

// What a constant-index access calls when its receiver is no dense Array.
const char* PropertyLoweringHelper::index_get_helper() const noexcept {
    return elem_stubs_ ? kIndexGetStub : "bronze_elem_get";
}

const char* PropertyLoweringHelper::index_set_helper() const noexcept {
    return elem_stubs_ ? kIndexSetStub : "bronze_elem_set";
}

void emit_elem_stubs(IlLowering* lowering, Module& mod) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    // A census counts every computed read in the helper (shape_census.h), so
    // its build keeps calling it.
    const bool here = !lowering->options().enable_census;
    pl.set_elem_stubs(here);
    if (!here || mod.get_function(kElemGetStub)) return;
    build_elem_get_stub(pl, mod.create_function(kElemGetStub, Type::i64(), {Type::i64(), Type::i64()}));
    build_elem_set_stub(pl, mod.create_function(kElemSetStub, Type::void_type(),
                                                {Type::i64(), Type::i64(), Type::i64(), Type::i32()}));
    build_index_get_stub(pl, mod.create_function(kIndexGetStub, Type::i64(), {Type::i64(), Type::i64()}));
    build_index_set_stub(pl, mod.create_function(kIndexSetStub, Type::void_type(),
                                                 {Type::i64(), Type::i64(), Type::i64(), Type::i32()}));
    // They answer what the helpers answer, operand for operand, so the passes
    // that read the helpers' contracts read theirs.
    mod.add_symbol_role(kElemGetStub, SymbolRole::ArrayGet);
    mod.add_symbol_role(kElemSetStub, SymbolRole::ArraySet);
    mod.add_symbol_role(kIndexGetStub, SymbolRole::ArrayGet);
    mod.add_symbol_role(kIndexSetStub, SymbolRole::ArraySet);
}

}  // namespace il2mir
