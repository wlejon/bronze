#include "runtime/elem_ic.h"

#include <bit>
#include <cstring>

#include "runtime/ic_log.h"
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

bool elemCacheEnabled() noexcept {
    return bronze_tls_block_addr()->elem_ic_enabled != 0;
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
    // absent entry outright, and nothing ever stores one here — see the fill
    // side for why absence is not cached on this path.
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

void elemCacheFill(const ElemProbe& probe, StringHeader* internedKey,
                   const InlineCacheSite& site) {
    if (!probe.entry || !internedKey) return;
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
    // Absence is the walk's to cache in a property SITE, whose key is a
    // compile-time constant that already exists. Here the key is a value, and
    // a key that names nothing need exist in the arena at all — so caching
    // absence would mean interning an immortal string for every fresh missing
    // name a loop asks for. The read stays uncached and the diagnostics on the
    // miss path keep running, which is the conservative half of both answers.
    if (filled.isAbsent()) return;
    probe.entry->ic = filled;
    probe.entry->witness = probe.witness;
    probe.entry->kind = probe.kind;
    probe.entry->key = internedKey;
}

}  // namespace bronze::runtime
