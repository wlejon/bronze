#include "runtime/elem_ic.h"

#include <bit>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/ic_log.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/shape_census.h"
#include "runtime/string.h"
#include "runtime/tls_block.h"

namespace bronze::runtime {

namespace {

// Direct-mapped and per thread. Per thread because a Shape is process-wide but
// an entry's validity is asked against `bronze_proto_epoch`, which lives in the
// per-thread ABI block — one shared table would have two threads answering each
// other's epoch questions. Zero-initialized, which is the empty state: an empty
// entry has a null shape, and `InlineCache::isRealShape` refuses it.
thread_local ElemCacheEntry g_elemCache[kElemCacheEntries];
thread_local ElemSetCacheEntry g_elemSetCache[kElemSetCacheEntries];

// Everything generated code assumes about the table's shape, asserted beside
// the table rather than beside the constants: a layout fact is only true where
// the type is, and a probe that reads the wrong offset does not fail loudly —
// it simply never hits.
static_assert(sizeof(ElemCacheEntry) == BRONZE_ABI_ELEM_ENTRY_SIZE);
static_assert(offsetof(ElemCacheEntry, ic) == BRONZE_ABI_ELEM_IC_OFFSET);
static_assert(offsetof(ElemCacheEntry, witness) == BRONZE_ABI_ELEM_WITNESS_OFFSET);
static_assert(offsetof(ElemCacheEntry, key) == BRONZE_ABI_ELEM_KEY_OFFSET);
static_assert(offsetof(ElemCacheEntry, kind) == BRONZE_ABI_ELEM_KIND_OFFSET);
static_assert(offsetof(ElemCacheEntry, beside_hits) == BRONZE_ABI_ELEM_BESIDE_HITS_OFFSET);
static_assert(offsetof(ElemCacheEntry, key_ident) == BRONZE_ABI_ELEM_IDENT_OFFSET);
static_assert(kElemCacheEntries == BRONZE_ABI_ELEM_ENTRIES);
static_assert(sizeof(ElemSetCacheEntry) == BRONZE_ABI_ELEM_ENTRY_SIZE);
static_assert(offsetof(ElemSetCacheEntry, ic) == BRONZE_ABI_ELEM_IC_OFFSET);
static_assert(offsetof(ElemSetCacheEntry, witness) == BRONZE_ABI_ELEM_WITNESS_OFFSET);
static_assert(offsetof(ElemSetCacheEntry, key) == BRONZE_ABI_ELEM_KEY_OFFSET);
static_assert(offsetof(ElemSetCacheEntry, kind) == BRONZE_ABI_ELEM_KIND_OFFSET);
static_assert(offsetof(ElemSetCacheEntry, key_ident) == BRONZE_ABI_ELEM_IDENT_OFFSET);
static_assert(kElemSetCacheEntries == BRONZE_ABI_ELEM_SET_ENTRIES);
// The string-key arm reads the key's memoized hash straight off the flags
// word, so the layout and the two masks are ABI now.
static_assert(offsetof(StringHeader, flags) == BRONZE_ABI_STRING_FLAGS_OFFSET);
static_assert(StringHeader::kHasHashFlag == BRONZE_ABI_STRING_HASHED_BIT);
static_assert((0xFFFFFFFFu & ~(StringHeader::kHasHashFlag | StringHeader::kUTF16Flag)) ==
              BRONZE_ABI_STRING_HASH_MASK);
static_assert((kElemCacheEntries & (kElemCacheEntries - 1)) == 0, "bucket masking needs a power of two");
static_assert(64 - std::countr_zero(kElemCacheEntries) == BRONZE_ABI_ELEM_BUCKET_SHIFT);
static_assert(64 - std::countr_zero(kElemSetCacheEntries) == BRONZE_ABI_ELEM_SET_BUCKET_SHIFT);
static_assert(static_cast<uint8_t>(ElemKeyKind::Number) == BRONZE_ABI_ELEM_KIND_NUMBER);
static_assert(static_cast<uint8_t>(ElemKeyKind::String) == BRONZE_ABI_ELEM_KIND_STRING);
static_assert(static_cast<uint8_t>(ElemKeyKind::Boolean) == BRONZE_ABI_ELEM_KIND_BOOL);

// Multiplicative (Fibonacci) hashing of both inputs, the bucket taken from
// the product's HIGH bits. Both inputs need the high bits rather than the low:
// a NUMBER witness is a DOUBLE's bit pattern, and a small integer's double has
// about forty ZERO low bits, so `factorToGL[2884]`, `[2929]` and `[2960]`
// differ only near the top of the word (a low-bits mask put all three in one
// bucket, evicting one another 15,000 times a frame on `many_meshes`); a
// shape pointer has the milder version of the same problem at the other end
// (arena alignment zeros at the bottom). A product's high bits depend on every
// bit at or below them, which covers both. The two products are independent,
// so generated code (il_elem_stub.cpp) computes the bucket in one multiply's
// latency rather than the two dependent finalizer rounds this used to be.
uint32_t bucketOf(const Shape* shape, uint64_t witness, uint32_t entries) noexcept {
    const uint64_t h = reinterpret_cast<uintptr_t>(shape) * BRONZE_ABI_ELEM_HASH_MUL_SHAPE +
                       witness * BRONZE_ABI_ELEM_HASH_MUL_WITNESS;
    return static_cast<uint32_t>(h >> (64 - std::countr_zero(entries)));
}

// The key's witness, or Empty for a kind this table does not speak for.
//
// A SYMBOL is refused because it has no string form and the name path this
// cache short-circuits never sees one; an OBJECT because 7.1.19 ToPropertyKey
// runs its `toString`, which is user code and must happen before anything is
// read; `null` and `undefined` because they are rare enough that an entry
// spent on them is an entry taken from a hot pair.
ElemKeyKind witnessFor(Value key, uint64_t& out) noexcept {
    if (key.isNumber()) {
        out = key.rawBits();
        return ElemKeyKind::Number;
    }
    if (key.isString()) {
        const StringHeader* s = key.asString<StringHeader>();
        if (!s) return ElemKeyKind::Empty;
        // Memoized in the flags word after the first call, so this is a load
        // on every read but the first for a given string object.
        out = s->hash();
        return ElemKeyKind::String;
    }
    if (key.isBool()) {
        out = key.asBool() ? 1u : 0u;
        return ElemKeyKind::Boolean;
    }
    return ElemKeyKind::Empty;
}

// Does this entry speak for this key? The shape half is asked separately, by
// `InlineCache::describes`, because that is the question the property path
// already owns.
//
// A template over the two entry types rather than two copies: the read table's
// entry carries an ident latch the write table has no use for, but the KEY
// question is the same question, and the one thing worse than two tables is
// two opinions about what a key is.
template <typename Entry>
bool keyMatches(const Entry& e, ElemKeyKind kind, uint64_t witness, Value key) noexcept {
    // An EMPTY entry is caught by the kind, which no live probe ever carries;
    // and for a NUMBER or BOOLEAN key the witness is not a filter but the whole
    // identity — the raw bits of the double, or 0/1 — so kind and witness
    // together settle it with nothing left to compare.
    if (e.kind != kind || e.witness != witness) return false;
    if (kind != ElemKeyKind::String) return true;
    if (!e.key) return false;
    // The hash agreed; the content decides. `e.key` is the arena copy, which
    // cannot move under the comparison, and the incoming string is the one the
    // caller still holds — no allocation happens between the two reads.
    const StringHeader* s = key.asString<StringHeader>();
    return s && (s == e.key || s->equals(*e.key));
}

}  // namespace

namespace {

// hash -> the arena copies made under it. A vector rather than one entry
// because the hash is a filter, not an identity, and two distinct keys sharing
// one must both be reachable. Keys here are ARENA copies: immortal, never
// moved, never scanned — so this map is a plain C++ container and no root
// source knows about it.
thread_local std::unordered_map<uint64_t, std::vector<StringHeader*>> g_elemKeyArena;
thread_local uint32_t g_elemKeyCount = 0;

}  // namespace

StringHeader* elemCacheInternKey(StringHeader* live) {
    if (!live) return nullptr;
    const uint64_t h = live->hash();
    auto found = g_elemKeyArena.find(h);
    if (found != g_elemKeyArena.end()) {
        for (StringHeader* candidate : found->second) {
            if (candidate == live || candidate->equals(*live)) return candidate;
        }
    }
    // The budget, and the one place it is spent. A program that computes a
    // fresh key per iteration reaches it, stops growing the arena, and keeps
    // the uncached read it always had — a slow answer, never a wrong one.
    // Checked before the table is touched: a bucket is made only for a key
    // that is kept, or every fresh key past the budget would still leave an
    // empty bucket behind for the life of the thread.
    if (g_elemKeyCount >= kElemKeyBudget) return nullptr;
    StringHeader* copy = StringHeader::internToArena(rtArena(), live);
    ++g_elemKeyCount;
    g_elemKeyArena[h].push_back(copy);
    return copy;
}

void elemCachePublish() noexcept {
    // Where generated code finds the table, published once per thread from
    // Heap's constructor, the thread's first touch of the runtime. A thread
    // that has not reached that point sees a null base, which is the inline
    // path's first refusal and costs it the helper it already had.
    rtTls()->elem_cache_tbl = reinterpret_cast<uint64_t*>(g_elemCache);
    rtTls()->elem_set_cache_tbl = reinterpret_cast<uint64_t*>(g_elemSetCache);
}

void elemCacheSweepIdent(uintptr_t lo, uintptr_t hi) noexcept {
    // Inside the collection pause, before the mutator resumes: clear every
    // ident naming a MOVABLE string. What survives is 0 or an arena string,
    // and neither can alias an address the next allocation cycle hands out —
    // which is invariant (2) of the inline guard. The payload mask, not a
    // tag check, because the word is either 0 (payload 0, outside any live
    // range) or a string Value this thread latched.
    for (ElemCacheEntry& e : g_elemCache) {
        const uintptr_t target =
            static_cast<uintptr_t>(e.key_ident & BRONZE_ABI_VALUE_PAYLOAD_MASK);
        if (target >= lo && target < hi) e.key_ident = 0;
    }
    for (ElemSetCacheEntry& e : g_elemSetCache) {
        const uintptr_t target =
            static_cast<uintptr_t>(e.key_ident & BRONZE_ABI_VALUE_PAYLOAD_MASK);
        if (target >= lo && target < hi) e.key_ident = 0;
    }
}

ElemProbe elemCacheProbe(Value objVal, Value key) {
    ElemProbe probe;
    // Census mode keeps every computed read reaching the helpers it records
    // in (shape_census.h).
    if (censusFillsSuppressed()) {
        recordElemIcMiss("census", objVal.rawBits(), key.rawBits());
        return probe;
    }
    // PLAIN receivers only, and that is a scope decision rather than a
    // limitation discovered late: an array, a typed array, a string exotic and
    // a function each answer a computed read from somewhere that is not the
    // ordinary shape walk — an element block, a synthesized index property, a
    // method table, a header field — and every one of those answers would have
    // to be re-derived here to be cached. The whole of the three.js bill this
    // was built for is a plain receiver, so the other kinds keep the path they
    // had and cost nothing to leave alone.
    if (!objVal.isObject()) {
        recordElemIcMiss("receiver_not_object", objVal.rawBits(), key.rawBits());
        return probe;
    }
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        recordElemIcMiss("receiver_kind_not_plain", objVal.rawBits(), key.rawBits());
        return probe;
    }
    Shape* shape = objVal.asObject<ObjectHeader>()->shape;
    if (!shape) {
        recordElemIcMiss("receiver_no_shape", objVal.rawBits(), key.rawBits());
        return probe;
    }
    // A dictionary object's properties are not in the shape at all, so a slot
    // number cached against one names nothing — unless it is frozen
    // (Shape::readCacheable). The property path refuses the same receivers
    // for the same reason.
    if (!shape->readCacheable()) {
        recordElemIcMiss("receiver_dictionary", objVal.rawBits(), key.rawBits());
        return probe;
    }

    uint64_t witness = 0;
    const ElemKeyKind kind = witnessFor(key, witness);
    if (kind == ElemKeyKind::Empty) {
        recordElemIcMiss("key_kind_uncacheable", objVal.rawBits(), key.rawBits());
        return probe;
    }

    // Two ways: the pair's bucket and the entry beside it (`bucket ^ 1`). Two
    // pairs of one bucket otherwise evict one another on every read, and which
    // pairs share a bucket is decided by where the shapes were allocated:
    // three.js's `geometryAttributes[name]` and `programAttributes[name]`
    // (needsUpdate, two reads a mesh a frame) did in most runs of one binary
    // and in none of the next one's, 0.8M instructions a frame apart.
    const uint32_t bucket = bucketOf(shape, witness, kElemCacheEntries);
    ElemCacheEntry* found = nullptr;
    for (ElemCacheEntry* way : {&g_elemCache[bucket], &g_elemCache[bucket ^ 1]}) {
        if (way->ic.cached_shape == shape && keyMatches(*way, kind, witness, key)) {
            found = way;
            break;
        }
    }
    probe.witness = witness;
    probe.kind = kind;
    if (found == nullptr) {
        // A fill goes to the bucket, the way generated code tests at the site
        // (elemCacheFill keeps what it displaces, where it can).
        ElemCacheEntry& first = g_elemCache[bucket];
        probe.entry = &first;
        recordElemIcMiss(first.kind == ElemKeyKind::Empty    ? "entry_empty"
                         : first.ic.cached_shape == shape ? "entry_same_shape_other_key"
                                                          : "entry_other_pair_collision",
                         objVal.rawBits(), key.rawBits());
        return probe;
    }
    // A pair the stub has answered from beside its bucket often enough takes
    // the bucket, and the pair that held it goes beside.
    if (found != &g_elemCache[bucket] && found->beside_hits >= BRONZE_ABI_ELEM_BESIDE_PROMOTE) {
        std::swap(g_elemCache[bucket], *found);
        found->beside_hits = 0;
        found = &g_elemCache[bucket];
        found->beside_hits = 0;
    }
    ElemCacheEntry& e = *found;
    probe.entry = found;

    // The identity latch. `keyMatches` just proved the LIVE key content-equal
    // to this entry's arena key — by `equals`, the loop the inline arm must
    // not carry — and that proof is the whole of what the arm's single
    // ident-vs-key-bits compare stands on. Latched on every helper string
    // hit, even one the shape or epoch questions below are about to refuse:
    // the invariant is about the KEY pair only, and the entry's shape words
    // answer for themselves on the next probe.
    if (kind == ElemKeyKind::String) {
        e.key_ident = key.rawBits();
    }

    // From here the questions are exactly the property path's, asked of its
    // own code so the two cannot answer differently. `describes` refuses an
    // absent entry outright — that is a separate question with a separate
    // answer, so it is asked separately, first.
    if (e.ic.isAbsent()) {
        if (!e.ic.describesAbsent(shape)) {
            recordElemIcMiss(e.ic.cached_shape == shape ? "entry_absent_epoch_stale"
                                                        : "entry_absent_other_shape",
                             objVal.rawBits(), key.rawBits());
            return probe;
        }
        // `undefined`, with no own-slot lookup and no chain walk. The entry's
        // validity is the receiver's shape (every own add transitions it) and
        // `bronze_proto_epoch` (every way a key can appear above it) — the
        // pair chunk 1 proved for the named negative IC, asked here of the
        // same words by the same code.
        probe.hit = true;
        probe.value = Value::fromUndefined();
        return probe;
    }
    if (!e.ic.describes(shape)) {
        recordElemIcMiss(e.ic.cached_shape == shape ? "entry_epoch_stale" : "entry_other_shape",
                         objVal.rawBits(), key.rawBits());
        return probe;
    }
    if (e.ic.isAccessor()) {
        // A getter is a CALL: it can allocate, it can throw, and it needs the
        // receiver rooted. Nothing about that belongs on a path whose contract
        // is "no allocation, no user code", so an accessor site keeps the
        // uncached read and the entry stays as a record that it did.
        recordElemIcMiss("entry_accessor", objVal.rawBits(), key.rawBits());
        return probe;
    }
    if (e.ic.cached_depth == 0) {
        probe.hit = true;
        probe.value = objVal.asObject<ObjectHeader>()->getSlot(e.ic.cached_slot);
        return probe;
    }
    bool crossedDictionary = false;
    ObjectHeader* holder =
        objVal.asObject<ObjectHeader>()->cachedProtoHolder(e.ic.cached_depth, crossedDictionary);
    if (!holder) {
        recordElemIcMiss("proto_walk_refused", objVal.rawBits(), key.rawBits());
        return probe;
    }
    probe.hit = true;
    probe.value = holder->getSlot(e.ic.cached_slot);
    return probe;
}

void elemCacheFill(const ElemProbe& probe, StringHeader* liveKey, const InlineCacheSite& site) {
    if (!probe.entry || !liveKey) return;
    // Way 0 or nothing. The walk installs move-to-front, so way 0 is what it
    // just decided about THIS receiver — and a stack site handed to one read
    // has no older ways for the scan to have found. Nothing here re-decides
    // cacheability: an entry the walk declined to fill leaves a null shape,
    // and that is the answer copied across.
    const InlineCache& filled = site.ways[0];
    if (!filled.isRealShape()) return;
    // An array-method sentinel cannot appear — the probe admits only plain
    // receivers — but the entry is copied verbatim, so the assertion is worth
    // the branch rather than the trust.
    if (filled.isArrayMethod()) return;
    // The key has to become immortal before an entry can name it, and for an
    // ABSENT key that is the whole risk: a present key is already a shape key
    // somewhere, an absent one need exist nowhere. `elemCacheInternKey`
    // deduplicates and spends a fixed budget, so a stable absent pair costs one
    // copy and a rotating one costs nothing after the budget.
    StringHeader* internedKey = elemCacheInternKey(liveKey);
    if (!internedKey) {
        recordElemIcMiss("key_budget_spent", 0, 0);
        return;
    }
    // The pair this fill displaces from its own bucket moves to the way
    // beside it, whatever is there: two pairs of one bucket then both answer
    // (the newer at the site, the older in the stub). A pair at home beside
    // them is put out for it, since a pair nothing reads any more would
    // otherwise hold the way for good and leave the two to displace each
    // other on every read; one that is read fills its bucket again, and what
    // it displaces then is not at home, so nothing moves twice.
    const size_t index = static_cast<size_t>(probe.entry - g_elemCache);
    const auto home = [](const ElemCacheEntry& e) {
        return bucketOf(e.ic.cached_shape, e.witness, kElemCacheEntries);
    };
    const bool same_pair = probe.entry->kind == probe.kind && probe.entry->witness == probe.witness &&
                           probe.entry->ic.cached_shape == filled.cached_shape &&
                           (probe.kind != ElemKeyKind::String || probe.entry->key == internedKey);
    if (probe.entry->kind != ElemKeyKind::Empty && !same_pair && home(*probe.entry) == index) {
        g_elemCache[index ^ 1] = *probe.entry;
        g_elemCache[index ^ 1].beside_hits = 0;
    }
    probe.entry->ic = filled;
    probe.entry->witness = probe.witness;
    probe.entry->kind = probe.kind;
    probe.entry->key = internedKey;
    // The ident is rewritten with the rest of the entry, in the same breath —
    // the first invariant the inline guard stands on. For a string fill it
    // names the LIVE key (`liveKey`, re-read through the caller's root after
    // the walk, so post-collection bits); `internedKey` content-equals it by
    // construction of `elemCacheInternKey`. For a number or boolean fill the
    // key that reached this function is the STRINGIFIED key, an object no
    // read site will ever present, so the word goes to zero rather than
    // carrying a pointer the sweep would have to chase for nothing.
    probe.entry->key_ident =
        probe.kind == ElemKeyKind::String
            ? Value::fromString(liveKey).rawBits()
            : 0;
}

ElemSetCacheEntry* elemSetCacheEntryFor(Value objVal, Value key) {
    // PLAIN receivers only, and for the read table's reason rather than a
    // narrower one: every other kind answers a computed store from somewhere
    // that is not the shape walk — an element block, a typed view's window, a
    // function's statics side object, a Map's property table, a proxy trap —
    // and a slot number cached against one of those names nothing. The
    // three.js and pixi computed stores this was built for are all plain.
    if (!objVal.isObject()) {
        recordElemIcMiss("set_receiver_not_object", objVal.rawBits(), key.rawBits());
        return nullptr;
    }
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        recordElemIcMiss("set_receiver_kind_not_plain", objVal.rawBits(), key.rawBits());
        return nullptr;
    }
    Shape* shape = objVal.asObject<ObjectHeader>()->shape;
    if (!shape) {
        recordElemIcMiss("set_receiver_no_shape", objVal.rawBits(), key.rawBits());
        return nullptr;
    }
    // A dictionary's properties are not in the shape, so a slot number cached
    // against one names nothing — and it is also where `Object.freeze`,
    // `Object.preventExtensions` and a non-writable `defineProperty` put an
    // object, which is what keeps every refusal `setProp` owes off this path
    // without this table having to know any of them by name.
    if (shape->dict) {
        recordElemIcMiss("set_receiver_dictionary", objVal.rawBits(), key.rawBits());
        return nullptr;
    }

    uint64_t witness = 0;
    const ElemKeyKind kind = witnessFor(key, witness);
    if (kind == ElemKeyKind::Empty) {
        recordElemIcMiss("set_key_kind_uncacheable", objVal.rawBits(), key.rawBits());
        return nullptr;
    }

    ElemSetCacheEntry& e = g_elemSetCache[bucketOf(shape, witness, kElemSetCacheEntries)];
    if (keyMatches(e, kind, witness, key)) {
        if (kind == ElemKeyKind::String) {
            e.key_ident = key.rawBits();
        }
        return &e;
    }

    recordElemIcMiss(e.kind == ElemKeyKind::Empty       ? "set_entry_empty"
                     : e.ic.cached_shape == shape ? "set_entry_same_shape_other_key"
                                                  : "set_entry_other_pair_collision",
                     objVal.rawBits(), key.rawBits());

    // Re-point the entry at THIS pair. A STRING key has to become immortal
    // before an entry can name it, because the entry outlives the live string
    // and the hash only filters — the content comparison on the next probe
    // needs a copy that cannot move under it. A program that computes a fresh
    // key per store spends the shared budget and then stops being cached.
    //
    // A NUMBER or BOOLEAN key needs no copy at all: its witness IS its
    // identity, so the entry keeps a null `key`. That is not a saving but a
    // requirement — the only way to reach a string for such a key is
    // `rtElemKeyAsString`, which allocates on the GC heap, and this function's
    // callers hold a RAW receiver across it.
    StringHeader* interned = nullptr;
    if (kind == ElemKeyKind::String) {
        interned = elemCacheInternKey(key.asString<StringHeader>());
        if (!interned) {
            recordElemIcMiss("set_key_budget_spent", objVal.rawBits(), key.rawBits());
            return nullptr;
        }
    }
    // Cleared in the same breath as the key is rewritten, and this is the line
    // the whole mechanism stands on: `setProp` asks `describesOwn`, which is a
    // question about the SHAPE only, so an `ic` left describing the previous
    // key would answer yes for a receiver of that shape and store this value in
    // that key's slot. `setProp` will fill it again for this pair, under its
    // own rules, before anything reads it.
    e.ic = InlineCache{};
    e.witness = witness;
    e.kind = kind;
    e.key = interned;
    e.key_ident = kind == ElemKeyKind::String ? key.rawBits() : 0;
    return &e;
}

}  // namespace bronze::runtime

extern "C" {

// A computed read's SITE CELL (il2mir lower_elem_get, "one pair"): four words
// of the site's own, `shape, key bits, slot word, fills`. Generated code
// answers `o[k]` from them with two compares where the table costs a hash and
// a probe: pixi reads `renderPipes[renderable.renderPipeId]` once a sprite a
// frame, and every one of those reads names one pair. Called on the site's
// miss, before the read; it runs no code of the program's and allocates
// nothing a collection would see.
//
// What the cell's two compares stand for, each a thing checked here:
//   - a plain receiver of that shape, and an own data slot (the table's
//     entry describes it, by the property path's own rules; a shape names
//     its own properties for good, so no epoch);
//   - the key, by its bits: a Number's are the Number (`sprite._gpuData[uid]`
//     names a string property by one, and the table answers it without the
//     string), and of strings an ARENA string only, which is immortal, so
//     its bits name the same characters for as long as the cell holds them,
//     on every thread. A heap string's bits do not survive a collection, and
//     this cell is not among what the collector sweeps.
// A site that names many pairs stops filling after kElemCellFills, and reads
// through the table as before.
void bronze_elem_cell_fill(uint64_t objBits, uint64_t keyBits, uint64_t* cell) {
    using namespace bronze;
    using namespace bronze::runtime;
    constexpr uint64_t kElemCellFills = 8;
    if (cell == nullptr || cell[3] >= kElemCellFills) return;
    const Value key(keyBits);
    const bool arena = key.isString() && key.asString<StringHeader>()->isArena();
    if (!arena && !key.isNumber()) return;
    const Value obj(objBits);
    if (!obj.isObject() || obj.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) return;
    auto* plain = obj.asObject<ObjectHeader>();
    Shape* shape = plain->shape;
    if (shape == nullptr || shape->isDictionary()) return;
    uint64_t slotWord = 0;
    PropertyInfo info;
    if (arena && plain->internalSlotCount() == 0) {
        // The shape answers for a name: no read need have come before.
        if (!shape->lookupProperty(PropertyKey::forString(key.asString<StringHeader>()), info) || info.accessor) return;
        slotWord = info.slot;
    } else {
        // What the table holds for the pair, from the read before this one.
        const ElemProbe probe = elemCacheProbe(obj, key);
        if (!probe.hit || probe.entry == nullptr || probe.entry->ic.cached_shape != shape) return;
        if (probe.kind != (arena ? ElemKeyKind::String : ElemKeyKind::Number)) return;
        std::memcpy(&slotWord,
                    reinterpret_cast<const char*>(probe.entry) + BRONZE_ABI_ELEM_IC_OFFSET + BRONZE_ABI_IC_SLOTWORD_OFFSET,
                    sizeof slotWord);
        if (slotWord >= (uint64_t{1} << 32)) return;
    }
    cell[0] = 0;
    cell[1] = keyBits;
    cell[2] = slotWord;
    cell[0] = reinterpret_cast<uint64_t>(shape);
    ++cell[3];
}

}  // extern "C"
