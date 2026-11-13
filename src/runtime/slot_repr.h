#pragma once

// PER-SLOT REPRESENTATION. docs/slot-representation.md is the long form.
//
// A property slot holds a `bronze::Value`, NaN-boxed, unless its shape says
// otherwise: a shape may say, per slot, that the slot's eight bytes ARE a
// double, and the runtime keeps that claim true against every store.
//
// WHAT THE CLAIM MEANS. `SlotRepr::Double` on a slot says: reading those eight
// bytes as an IEEE double is correct, with no tag test and no branch. It is NOT
// a promise about what the program wrote most recently — the runtime is free to
// take the claim back, and does, the moment a store contradicts it.
//
// HOW IT IS TAKEN BACK: GENERALIZATION. The invalidation mechanism is SHAPE
// IDENTITY. A double slot that receives a non-number moves its object to a
// shape whose node for that slot is boxed (`Shape::withSlotBoxed`). Shape nodes
// are immutable once created, so every OTHER object still at the old shape is
// untouched and still holds a double there; inline caches keyed on the old
// shape simply stop matching for the object that moved. It is the same
// discipline an attribute change uses, applied to one more fact about a slot.
//
// WHY A DOUBLE SLOT IS BIT-COMPATIBLE WITH A BOX. bronze NaN-boxes directly: a
// Number's Value bits ARE the double's bits (value.h), with NaN canonicalized.
// So a double slot written through this module holds a word that also parses as
// a number-tagged Value, and every reader that does not know about
// representations — the collector's generic payload scan, an inline cache's
// slot load — gives the right answer. The one thing it costs is the
// non-canonical NaN a raw double slot could otherwise carry, which nothing
// wants.
//
// HOW EVERY STORE IS HELD TO IT. Every write goes through
// `ObjectHeader::setSlot`, which either canonicalizes a number into the slot or
// generalizes the slot away — one choke point, so a store path added later
// inherits the discipline instead of escaping it. Generated code's `prop.set`
// is a call to `bronze_prop_set`, which reaches `setSlot` like every other
// path. A set-site inline cache entry naming a double slot carries
// BRONZE_ABI_IC_DEPTH_DOUBLE_FLAG (`ICEntry::isDoubleSlot`): the test an inline
// store arm in generated code would make before taking the entry. The runtime's
// store path does not read it, since it reaches `setSlot` in every case.
//
// WHERE A DOUBLE SLOT COMES FROM. Only from an ELIGIBLE key whose first store
// is a number. Eligibility is handed over at module init as a list of names
// (`bronze_register_slot_repr`), which the compiler takes from the `--pins`
// manifest (a field pinned `number` on a class whose layout the compilation
// proved) and from an inline-cache harvest (keys the harvested run's objects
// were born and kept Numbers under; docs/slot-representation.md "From a
// harvest"). Any other key is never eligible: a key that alternates would cost
// a shape split each time it turned over.
//
// THE OBJECT REPRESENTATION. The same discipline for one more fact: a slot a
// shape calls an OBJECT one holds an Object-tagged pointer to a plain object
// (HeapKind::Plain), so a value read from it can be used as a receiver with
// neither the tag test nor the header test. The eight bytes are an ordinary
// boxed Value (the collector traces them as any other); what the shape adds is
// that every store keeps the claim or moves the object off the shape
// (`setSlot`, `Shape::withSlotBoxed`), and that generated code's store arms
// test the value before they take an entry or a cell naming such a slot. The
// held object's own shape is no part of the claim: it may change with no
// store to the holder. Born only from an eligible key whose first store is a
// plain object; eligibility comes from a harvest (keys the harvested run's
// objects were born and kept plain objects under).

#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/heap.h"
#include "runtime/property_key.h"
#include "runtime/value.h"

namespace bronze {

class Shape;
struct StringHeader;

// How the eight bytes of one slot are to be read, and what a store must bring.
enum class SlotRepr : uint8_t {
    Boxed = BRONZE_ABI_SLOT_REPR_BOXED,
    Double = BRONZE_ABI_SLOT_REPR_DOUBLE,
    Object = BRONZE_ABI_SLOT_REPR_OBJECT,
};

// Slots at or above this index are never given a representation: the shape's
// summary is one 64-bit word, and a clear bit has to be a truthful "boxed"
// rather than "did not fit". Objects with more than 64 own properties are not
// what this stage is about.
constexpr uint32_t kSlotReprLimit = BRONZE_ABI_SHAPE_DOUBLE_SLOT_LIMIT;

// Is this value one a double slot may hold? Both spellings of a Number count:
// the `Tag::Int32` box a bitwise operation produces is a Number too (ECMA-262
// 6.1.6.1), and refusing it would generalize a slot for `x = y | 0`.
inline bool slotReprAcceptsValue(Value v) noexcept {
    return v.isNumber() || v.tag() == static_cast<uint16_t>(Tag::Int32);
}

// Is this value one an object slot may hold? Exactly what generated code's
// receiver tests ask: an Object-tagged Value whose header says a plain object.
// The header's kind never changes over an object's life, so a value that was
// one when stored is one when read.
inline bool slotReprAcceptsObject(Value v) noexcept {
    if (!v.isObject()) return false;
    const auto* hdr = v.asObject<HeapObjectHeader>();
    return hdr->tag == static_cast<uint16_t>(Tag::Object) && hdr->flags == HeapKind::Plain;
}

// Whether a slot of representation `repr` takes `v` as it is (a double slot
// canonicalizes it, setSlot).
inline bool slotReprFits(SlotRepr repr, Value v) noexcept {
    switch (repr) {
        case SlotRepr::Double: return slotReprAcceptsValue(v);
        case SlotRepr::Object: return slotReprAcceptsObject(v);
        case SlotRepr::Boxed: break;
    }
    return true;
}

// The double a double slot stores for `v`, which `slotReprAcceptsValue` has
// already accepted. Canonicalizing NaN is what keeps the stored word a legal
// Value for every reader that has not learned about representations yet.
inline Value slotReprCanonicalize(Value v) noexcept {
    if (v.isNumber()) return Value::fromDouble(v.asNumber());
    return Value::fromDouble(static_cast<double>(static_cast<int32_t>(v.payload())));
}

}  // namespace bronze

namespace bronze::runtime {

// For tests: forgets every registered eligible key and zeroes the counters.
void slotReprResetForTesting();

// May a slot first installed under `key` be born `repr` (Double or Object)?
// The name list `bronze_register_slot_repr` handed over, each name with the
// representations it is eligible for. Asked once per shape NODE CREATION — a
// cold path — so a linear scan over the (few dozen) registered names is the
// whole implementation.
bool slotReprEligible(PropertyKey key, SlotRepr repr = SlotRepr::Double) noexcept;

// Makes one name eligible for `repr`. Idempotent by content. Called by
// `bronze_register_slot_repr` and by tests.
void slotReprRegisterName(StringHeader* name, SlotRepr repr = SlotRepr::Double);
uint32_t slotReprEligibleCount() noexcept;

// --- diagnostics ------------------------------------------------------------
//
// Two modes, documented in docs/shape-census.md:
//
//   BRONZE_SLOT_REPR_STATS=1   the free counters below, printed at exit. They
//                              are collected unconditionally because every one
//                              of them is incremented on a cold path (a shape
//                              node being created, a slot generalizing), so
//                              collecting them costs a normal run nothing.
//
//   BRONZE_SLOT_REPR_CENSUS=1  per-(shape, slot) REPRESENTATION STABILITY: for
//                              every slot of every shape the run touched, how
//                              many stores were numbers and how many were not.
//                              A boxed slot whose stores are 100% numbers is a
//                              slot a wider pins manifest could claim. It needs the shape census's
//                              latch suppression to see inline-cache hit
//                              traffic, so it turns that on too, and a census
//                              run is counts and never times.
struct SlotReprCounters {
    uint64_t double_nodes = 0;      // shape nodes created with SlotRepr::Double
    uint64_t object_nodes = 0;      // shape nodes created with SlotRepr::Object
    uint64_t boxed_nodes = 0;       // shape nodes created boxed
    uint64_t refused_ineligible = 0;  // a number store whose key was not eligible
    uint64_t generalizations = 0;   // double or object slots taken back by a store
    uint64_t generalized_nodes = 0;  // distinct double or object nodes marked sticky-boxed
    // Stores that landed in a double slot THROUGH `setSlot` — the runtime's
    // paths and generated code's misses. Generated code's inline arms test the
    // value and store it themselves, so they are not counted here and a hot
    // pinned field's count is near zero once its sites have latched. That is
    // the number reading small, not the mechanism being idle: `double_nodes`
    // is what says the slots exist.
    uint64_t double_stores = 0;
};
const SlotReprCounters& slotReprCounters() noexcept;
SlotReprCounters& slotReprMutableCounters() noexcept;

bool slotReprCensusEnabled() noexcept;
// One observed property access on a plain receiver, from the shape census's
// recording point. `stored` is meaningful only when `hasValue`.
void slotReprCensusNote(const Shape* shape, PropertyKey key, bool hasValue, Value stored);

// The text report both modes print. Public so a test can force one.
void slotReprReport();

}  // namespace bronze::runtime
