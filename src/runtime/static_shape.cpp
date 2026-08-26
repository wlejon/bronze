// Reconciling a compile-time layout with a run-time shape.
//
// A property site whose class layout proved a constant instance slot compiles
// to a single pointer compare against a module-global CELL, then a load at that
// constant offset. Nothing in the compiler can know what pointer to put in the
// cell: shapes are minted at run time, by the transition tree, from the writes
// the program actually performs. This is the one function that fills it, and it
// is the whole reason the fast path is allowed to skip every other check the
// inline cache makes.
//
// The cell has three states, and the encoding is chosen so that generated code
// needs no extra test to tell them apart:
//
//   0                        never tried. Cannot equal a live shape, so the
//                            guard misses and the site takes its slow path,
//                            which is what calls this.
//   kRefused (1)             tried once and the layout was wrong for the object
//                            that arrived. Also cannot equal a live shape (a
//                            shape is 8-aligned arena memory), so the guard
//                            still just misses — and the slow path stops
//                            calling this, so a wrong layout costs one probe
//                            and then nothing.
//   anything else            the `Shape*` this site expects.
//
// Publishing is deliberately ONE-SHOT and monomorphic. A site that sees two
// shapes pins the first and misses on the rest, exactly as a one-way cache
// would; churning the cell would turn a polymorphic site into a store on every
// execution, which is worse than the miss.
//
// Shapes are arena-allocated, immortal and non-moving. That is what makes a raw
// `Shape*` safe to keep in a module global with no GC visit — the same property
// the inline caches already depend on for their cached shape words, and the
// reason the collector's payload scan can read one as a number (its top 16 bits
// are clear) and walk past it.

#include "abi/bronze_abi.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/shape_census.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {
// Not a legal shape address: shapes come from the arena 8-aligned, so no live
// shape can ever compare equal to this, and the guard needs no extra test to
// treat a refusal as a miss.
constexpr uint64_t kRefused = 1;
}  // namespace

extern "C" {

// `slot` is the layout's claim, `forWrite` says whether the site stores.
//
// Every condition below is one the fast path will NOT re-check, so each is a
// thing the shape word has to stand for afterwards:
//
//   - an object receiver, and a PLAIN one. Arrays, functions, typed arrays and
//     proxies answer named properties through their own machinery, and their
//     header does not carry a shape at all at this offset.
//   - not a dictionary. A dictionary shape belongs to ONE object, so pinning it
//     would pin a single instance, and its slot numbering is a run-time fact.
//   - the key is an OWN property. A prototype hit has no receiver slot to name,
//     which is why lowering never asks for one (see `claimStaticSlot`).
//   - a DATA property, not an accessor. An accessor occupies two slots and its
//     read is a call.
//   - at exactly the slot the layout claimed. This is the check that makes a
//     wrong layout a cost instead of a wrong answer, and the reason nothing
//     upstream of here needs to be sound.
//   - WRITABLE when the site stores, since the fast path is a bare store and
//     cannot fall into 10.4.5's refusal.
void bronze_static_shape_publish(uint64_t objBits, uint32_t keyIndex, uint64_t* cell,
                                 uint32_t slot, bool forWrite) {
    recordHelperCall("bronze_static_shape_publish");
    // Census mode: never publish, so the static guard keeps missing into the
    // ordinary sequence and its traffic reaches bronze_prop_get / _set, where
    // the census records it. The emitter calls-and-continues (no retry loop),
    // so a permanently-zero cell costs one helper call per access and nothing
    // else — the census's stated price.
    if (censusFillsSuppressed()) return;
    if (cell == nullptr || *cell != 0) return;

    Value objVal(objBits);
    if (!objVal.isObject()) {
        *cell = kRefused;
        return;
    }
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Plain) {
        *cell = kRefused;
        return;
    }
    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    Shape* shape = obj->shape;
    if (shape == nullptr || shape->isDictionary()) {
        *cell = kRefused;
        return;
    }
    StringHeader* key = rtKeyHeader(keyIndex);
    if (key == nullptr) {
        *cell = kRefused;
        return;
    }
    PropertyInfo info;
    if (!shape->lookupProperty(PropertyKey::forString(key), info)) {
        *cell = kRefused;
        return;
    }
    if (info.accessor || info.slot != slot || (forWrite && !info.writable)) {
        *cell = kRefused;
        return;
    }
    // A WRITE site at a slot the shape calls a double. The site's fast path is
    // a bare store of whatever bits it holds, and the slot's representation
    // says those bits are an f64 — so publishing here would licence a store
    // that can put a pointer in a double slot, which is the one thing the
    // representation must never be able to be wrong about (slot_repr.h).
    // Refusing costs this site its inline store and nothing else: the miss
    // reaches `bronze_prop_set`, whose `ObjectHeader::setSlot` handles the slot
    // correctly and generalizes it if the value is not a number.
    //
    // A READ site is published as before. Reading a double slot as a Value is
    // right in stage R1 — the stored word is the number's box (slot_repr.h) —
    // so the hottest half of a pinned field keeps its constant-offset load.
    if (forWrite && shape->slotIsDouble(slot)) {
        *cell = kRefused;
        return;
    }
    *cell = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(shape));
}

}  // extern "C"

}  // namespace bronze::runtime
