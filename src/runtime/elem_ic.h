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
    InlineCache ic;
    uint64_t witness = 0;
    // The key this entry is about, ARENA-interned so it is immortal. Null when
    // the entry is empty. Also what a HIT hands the walk on the miss path, so
    // no second interning happens per read.
    StringHeader* key = nullptr;
    ElemKeyKind kind = ElemKeyKind::Empty;
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
// claims — so nothing here re-decides cacheability; it copies way 0 across
// when the walk chose to fill it, and does nothing when it did not.
void elemCacheFill(const ElemProbe& probe, StringHeader* internedKey,
                   const InlineCacheSite& site);

// BRONZE_NO_ELEM_IC=1. Read through the per-thread ABI block like every other
// seam, so one binary can A/B.
bool elemCacheEnabled() noexcept;

}  // namespace bronze::runtime
