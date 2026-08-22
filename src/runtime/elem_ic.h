#pragma once

#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace bronze {
// Declared in runtime/string.h; only ever a pointer here, and the header this
// sits in is included by the property paths, which already have the full type.
struct StringHeader;
}  // namespace bronze

namespace bronze::runtime {

// The cache a COMPUTED read consults — `o[k]`, where the key is a value and
// not a name the compiler baked into a site.
//
// Why this is not the property site cache. A property site's entry is keyed on
// the receiver's SHAPE alone, and that is sound only because the key is a
// compile-time constant belonging to the site. A computed read has no such
// constant: two evaluations of one source position can name two properties, so
// an entry has to pin the KEY as well as the shape, and the site table has no
// word to pin it in. Rather than widen every property entry by a word for the
// small minority of reads that are computed, computed reads get their own
// table — and because they have no per-site word at all (the helper takes no
// site pointer), the table is keyed by (shape, key) and shared, direct-mapped,
// per thread.
//
// The three.js bill this exists for, measured on `many_meshes` (360 frames):
// 32.5 M helper entries, 100 % of them a PLAIN receiver, and every one of them
// uncached — `bronze_elem_get` funnelled the whole lot into `propGetByName`
// with `ic = nullptr`. The two shapes it takes:
//   * a NUMBER key naming a string property — `factorToGL[blendSrc]`,
//     `equationToGL[blendEquation]` in WebGLState, 14.4 M entries at ~80 ns,
//     most of that cost being the key STRING the old path materialised on
//     every read;
//   * a STRING key — `geometry.attributes[name]`, `uniforms[name]`, 16.2 M
//     entries at ~47 ns.
// A number key never becomes a string on a hit here, which is why the two
// buckets converge rather than the first staying dearer than the second.
//
// GC: an entry holds a `Shape*` (immortal, non-moving arena), an ARENA-interned
// `StringHeader*` (immortal for the same reason a shape key is), and integers.
// No `Value`, no movable pointer, nothing the collector has to know about —
// exactly the discipline the property site table keeps, and for the same
// reason: this table is never scanned.

// What the entry's witness word means. A key of any other kind is not cached:
// a symbol has no string form to reach the name path with, and an object key
// runs user code in ToPropertyKey before the read even begins.
enum class ElemKeyKind : uint8_t {
    Empty = 0,
    // `witness` is the raw bits of the key's double. Compared bitwise, so -0
    // and +0 land in different entries even though they name one property —
    // a duplicate entry is a miss at worst, never a wrong answer.
    Number = 1,
    // `witness` is the key string's memoized hash (StringHeader::hash, cached
    // in the flags word, so asking is a load). The hash is a FILTER and not the
    // identity: a hit is confirmed by comparing content against `key`, which is
    // the arena copy and cannot move under the comparison.
    String = 2,
    // `witness` is 0 or 1. `false` really is a property name — three.js reads
    // one 5,000 times a frame — and it costs one more enum value to keep it
    // off the string path.
    Boolean = 3,
};

struct ElemCacheEntry {
    // The answer, in exactly the representation a property site's entry uses:
    // shape, slot, depth (with the accessor and absent flags), fill epoch. It
    // is the same struct rather than a parallel one so that the validity
    // questions — `describes`, `describesAbsent`, `cachedProtoHolder`'s
    // preconditions — are asked of the same code the property path asks, and
    // cannot drift from it.
    // An ABSENT entry lives here too — `InlineCache::isAbsent()`, filled by
    // the same `rtInstallAbsentEntry` the named path uses, answered by the
    // same `describesAbsent`. It is the single largest line item chunk 3 left
    // behind: 1.80 M of that chunk's 1.80 M residual misses were `entry_empty`
    // on STABLE (shape, key) pairs — computed reads of a key that is absent
    // and stays absent, which under a present-only cache can never fill.
    InlineCache ic;
    uint64_t witness = 0;
    // The key this entry is about, ARENA-interned so it is immortal. Null when
    // the entry is empty. Also what a HIT hands the walk on the miss path, so
    // no second interning happens per read.
    StringHeader* key = nullptr;
    ElemKeyKind kind = ElemKeyKind::Empty;
    // The IDENTITY latch the inline string arm guards on: the raw Value bits
    // of the last LIVE string key `equals` proved content-equal to `key`, or
    // 0. Generated code compares its key bits against this ONE word and, on
    // equality, trusts the entry — sound under two invariants the runtime
    // keeps: (1) every write that changes what the entry is about rewrites
    // this word in the same breath (elemCacheFill; zero for a non-string
    // kind), and a re-latch happens only after `equals` confirmed the live
    // key against THIS entry's `key`; (2) elemCacheSweepIdent clears, inside
    // every collection pause, each ident pointing into the movable
    // reservation — the Cheney collector reuses an address only across a
    // collection, so an ident that survives to compare equal still names the
    // very object it was latched from. An ident naming an ARENA string (a
    // shape key handed out by enumeration) is immortal and survives the
    // sweep, which is what keeps the hot three.js pairs latched across GC.
    uint64_t key_ident = 0;
};

// Enough entries that the fifteen (shape, key) pairs three.js touches in its
// inner loop do not evict one another, and small enough to stay inside L1.
// Direct-mapped: a collision costs a fill, never a wrong answer.
inline constexpr uint32_t kElemCacheEntries = 4096;

// The probe's answer. `Hit` means `value` is the read's result and nothing
// else need happen; every other outcome falls through to the uncached path,
// and `reason` is what ic_log attributes the fall-through to.
struct ElemProbe {
    bool hit = false;
    Value value{Value::fromUndefined()};
    ElemCacheEntry* entry = nullptr;  // where a fill would go, or null to refuse
    uint64_t witness = 0;
    ElemKeyKind kind = ElemKeyKind::Empty;
};

// Look for `(objVal's shape, key)`. Never allocates and never runs user code,
// so the caller may hold a raw receiver across it.
ElemProbe elemCacheProbe(Value objVal, Value key);

// Record what a completed uncached read found, from the single-entry site the
// caller handed the walk. `site` is a stack `InlineCacheSite` the walk filled
// by its own rules — dictionary refusal, `chainIsCacheable`, the diagnostic
// claims, the index-like-key and `length` refusals — so nothing here
// re-decides cacheability; it copies way 0 across when the walk chose to fill
// it, and does nothing when it did not. That is the whole of why an ABSENT
// entry is sound on this path: absence reaches way 0 only through
// `rtInstallAbsentEntry`, which is the same gate the named-property negative
// IC passes, and the entry carries the same (shape, epoch) validity.
//
// `liveKey` is the key string AS IT IS NOW — the caller must re-read it
// through its root after the walk, because the walk can allocate. The arena
// copy this table keys on is made here, so no caller has to know the budget.
void elemCacheFill(const ElemProbe& probe, StringHeader* liveKey, const InlineCacheSite& site);

// Publish this thread's table into `bronze_tls_block::elem_cache_tbl`, so that
// generated code can probe it without a call (codegen-llvm/llvm_elem_cache.cpp
// emits the committed hit for a NUMBER or BOOLEAN key by witness, and for a
// STRING key by the `key_ident` identity latch). Called from Heap's
// constructor, which is the per-thread first touch.
void elemCachePublish() noexcept;

// BRONZE_NO_ELEM_IC=1. Read through the per-thread ABI block like every other
// seam, so one binary can A/B.
bool elemCacheEnabled() noexcept;

// BRONZE_NO_ELEM_ABSENT=1, the ABSENT half. Its own seam because the present
// half is chunk 3's and has to stay measurable alone: with this off, an absent
// pair falls back to the walk it always took and every present pair is
// untouched.
bool elemAbsentEnabled() noexcept;

// BRONZE_NO_ELEM_KEY_IC=1, the string-key IDENTITY latch. Latch-side, like
// the method-IC seams: with it off no fill or hit ever writes a non-zero
// `key_ident`, so the inline string arm can only miss into the helper it
// always took, and one binary A/Bs the mechanism against its own absence.
bool elemKeyIcEnabled() noexcept;

// Clear every `key_ident` that points into the movable heap — the range
// [lo, hi) is the reservation covering both semispaces. Registered by Heap's
// constructor as a post-collection hook, and running it inside the pause is
// the entire soundness story of the ident guard: an address is reused only
// across a collection, so after each collection no surviving ident can alias
// a recycled address. Idents OUTSIDE the range are immortal arena strings
// and stay latched.
void elemCacheSweepIdent(uintptr_t lo, uintptr_t hi) noexcept;

// The arena copy of `live` this table will key an entry on, or null when the
// key budget is spent.
//
// Why a table and not a bare copy. `StringHeader::internToArena` is a COPY,
// not a hash-cons — it grows the arena by the string's bytes on every call —
// and the arena is immortal. That was harmless while only PRESENT keys were
// cached, because a present key is already a shape key and a fill is rare; it
// is not harmless for absence, whose key need exist nowhere, so a loop asking
// for fresh missing names would grow the arena once per iteration. This
// deduplicates, so a repeated key costs one copy for the process, and refuses
// past `kElemKeyBudget` distinct keys, so a rotating one costs a bounded
// number and then simply stops being cached. Both halves of the cache use it.
StringHeader* elemCacheInternKey(StringHeader* live);

// How many distinct keys the table above will ever copy into the arena.
// three.js touches a couple of hundred; the budget exists for the program that
// does not.
inline constexpr uint32_t kElemKeyBudget = 8192;

}  // namespace bronze::runtime
