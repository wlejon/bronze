#include "runtime/elem_ic.h"

#include <bit>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "runtime/ic_log.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"

namespace bronze::runtime {

namespace {

// Direct-mapped and per thread. Per thread because a Shape is process-wide but
// an entry's validity is asked against `bronze_proto_epoch`, which lives in the
// per-thread ABI block — one shared table would have two threads answering each
// other's epoch questions. Zero-initialized, which is the empty state: an empty
// entry has a null shape, and `InlineCache::isRealShape` refuses it.
thread_local ElemCacheEntry g_elemCache[kElemCacheEntries];

// splitmix64's finalizer. Both inputs need it and one of them needs it badly:
// a NUMBER witness is a DOUBLE's bit pattern, and a small integer's double has
// about forty ZERO low bits. A mix that only multiplies leaves those zeros in
// the low half of the product, so `factorToGL[2884]`, `[2929]` and `[2960]`
// all indexed the same bucket and evicted one another 15,000 times a frame —
// 5.4 M misses on `many_meshes`, which read as key rotation until the reason
// was split by shape. A shape pointer has the milder version of the same
// problem at the other end (arena alignment zeros at the bottom), so both go
// through the finalizer rather than only the one that was caught.
uint64_t mix64(uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

uint32_t bucketOf(const Shape* shape, uint64_t witness) noexcept {
    const uint64_t h = mix64(reinterpret_cast<uintptr_t>(shape) ^ mix64(witness));
    return static_cast<uint32_t>(h) & (kElemCacheEntries - 1);
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
bool keyMatches(const ElemCacheEntry& e, ElemKeyKind kind, uint64_t witness,
                Value key) noexcept {
    if (e.kind != kind || e.witness != witness || !e.key) return false;
    if (kind != ElemKeyKind::String) return true;
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
    auto& bucket = g_elemKeyArena[h];
    for (StringHeader* candidate : bucket) {
        if (candidate == live || candidate->equals(*live)) return candidate;
    }
    // The budget, and the one place it is spent. A program that computes a
    // fresh key per iteration reaches it, stops growing the arena, and keeps
    // the uncached read it always had — a slow answer, never a wrong one.
    if (g_elemKeyCount >= kElemKeyBudget) return nullptr;
    StringHeader* copy = StringHeader::internToArena(rtArena(), live);
    ++g_elemKeyCount;
    bucket.push_back(copy);
    return copy;
}

bool elemCacheEnabled() noexcept {
    return bronze_tls_block_addr()->elem_ic_enabled != 0;
}

bool elemAbsentEnabled() noexcept {
    return bronze_tls_block_addr()->elem_absent_enabled != 0;
}

ElemProbe elemCacheProbe(Value objVal, Value key) {
    ElemProbe probe;
    if (!elemCacheEnabled()) {
        recordElemIcMiss("seam_disabled", objVal.rawBits(), key.rawBits());
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
    // number cached against one names nothing. The property path refuses the
    // same receiver for the same reason.
    if (shape->dict) {
        recordElemIcMiss("receiver_dictionary", objVal.rawBits(), key.rawBits());
        return probe;
    }

    uint64_t witness = 0;
    const ElemKeyKind kind = witnessFor(key, witness);
    if (kind == ElemKeyKind::Empty) {
        recordElemIcMiss("key_kind_uncacheable", objVal.rawBits(), key.rawBits());
        return probe;
    }

    ElemCacheEntry& e = g_elemCache[bucketOf(shape, witness)];
    probe.entry = &e;
    probe.witness = witness;
    probe.kind = kind;

    if (!keyMatches(e, kind, witness, key)) {
        recordElemIcMiss(e.kind == ElemKeyKind::Empty      ? "entry_empty"
                         : e.ic.cached_shape == shape ? "entry_same_shape_other_key"
                                                      : "entry_other_pair_collision", objVal.rawBits(), key.rawBits());
        return probe;
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
    if (filled.isAbsent() && !elemAbsentEnabled()) {
        recordElemIcMiss("absent_seam_disabled", 0, 0);
        return;
    }
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
    probe.entry->ic = filled;
    probe.entry->witness = probe.witness;
    probe.entry->kind = probe.kind;
    probe.entry->key = internedKey;
}

}  // namespace bronze::runtime
