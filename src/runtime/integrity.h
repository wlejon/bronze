#pragma once

#include <cstdint>

#include "runtime/array.h"
#include "runtime/dictionary.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace bronze::runtime {

// [[Extensible]] and the attributes SetIntegrityLevel stamps on an object's own
// properties — ECMA-262 7.3.14 and 7.3.15, spelled `Object.freeze`,
// `Object.seal`, `Object.preventExtensions` and the three predicates.
//
// WHERE THE LEVEL LIVES, and why it is there.
//
// A plain object records it in its own Dictionary: `extensible` is the bit and
// the per-entry `writable` / `configurable` cover its properties. Everything
// below is about the kinds that own storage a dictionary cannot describe.
//
// An ARRAY and a FUNCTION each already keep a side object for their NAMED
// properties (`ArrayHeader::properties`, `FunctionHeader::properties`), and
// freezing has to reach it anyway — a function's statics ARE its own
// properties, and a match array's `index` and `input` are its. So the level
// goes in THAT object's dictionary, and neither the array nor the function
// grows a field: an array that is never frozen pays nothing, and the one load
// the element write path gains is of a word already in the header's cache line.
// What a dictionary entry cannot describe — an array's elements, a function's
// `prototype` slot — is what `Dictionary::level` is for.
//
// Deliberately NOT the heap object header. Its `flags` word is not a flags
// word: it is the `HeapKind` registry (heap.h), read by three dozen equality
// tests across the runtime and by the plain-object check codegen-llvm inlines
// into every property site. Carving attribute bits out of it would turn every
// one of those into a masked compare, and a missed one would not fail — it
// would dispatch a frozen array as an unknown kind, or a frozen plain object as
// neither, which is the collision heap.h's registry exists to have named once.
//
// A Map, a Set, a typed array, an ArrayBuffer and a RegExp have no such side
// object and so no level at all. Every operation that would set one on them is
// a hard error by name (integrity.cpp), which is what makes the predicates'
// answers for them TRUE rather than merely convenient: the only route to
// non-extensible is refused loudly, so "extensible" is a fact and not a guess.
//
// A MODULE NAMESPACE has no side object either and is nonetheless not refused,
// because its state is decided by its KIND rather than by anything done to it.
// 10.4.6.3 [[IsExtensible]] returns false unconditionally and 10.4.6.5 makes
// every export non-configurable, so a namespace is non-extensible and sealed
// from birth and there is no bit for a level to be recorded in. That inverts
// the rule above rather than bending it: the refusal exists because a level
// nothing could read back would be a no-op reported as a success, and here
// there is no level to record and nothing to report.

// The table `obj` records its integrity level in, or null when nothing has ever
// set one — which for every kind that can have one means "extensible, open".
// No allocation, so the pointer is good until the next one.
Dictionary* rtIntegrityTable(Value obj);

inline bool rtIsExtensible(Value obj) {
    const Dictionary* d = rtIntegrityTable(obj);
    return !d || d->extensible;
}

inline IntegrityLevel rtIntegrityLevel(Value obj) {
    const Dictionary* d = rtIntegrityTable(obj);
    return d ? d->level : IntegrityLevel::Open;
}

// Why a write of `a[index]` cannot happen, or `SetRefusal::None`.
//
// An index the array does not already have as an own property — one at or past
// `length`, and one a `delete` punched a hole in — is a CREATE and needs
// [[Extensible]] (10.1.6.3 step 2.b). One it does have is a write and needs the
// elements to be writable, which is what `Object.freeze` takes away.
SetRefusal rtArrayElementWriteRefusalSlow(Value arrVal, uint32_t index);

// The same question, asked by every element write in the program — so the
// answer for an array that has never met `Object.freeze` is one load and a
// not-taken branch, off a field in the header's own cache line. That is the
// storage decision above paying for itself: an array with no side object has
// nowhere to have recorded a level, so there is nothing to consult.
inline SetRefusal rtArrayElementWriteRefusal(Value arrVal, uint32_t index) {
    if (!arrVal.asObject<ArrayHeader>()->properties.isObject()) return SetRefusal::None;
    return rtArrayElementWriteRefusalSlow(arrVal, index);
}

// Whether `delete a[i]` may remove an own element: false once the array is
// SEALED, which is the only thing that makes an element non-configurable.
bool rtArrayElementsConfigurable(Value arrVal);

// Whether `f.prototype = v` may proceed. A function's `prototype` is
// non-configurable but WRITABLE (10.2.4), and freezing is what takes the second
// half away — it lives in a slot rather than in the statics table, so nothing
// in that table can answer for it.
bool rtFunctionPrototypeWritable(Value fnVal);

// The six `Object` members that are SetIntegrityLevel and TestIntegrityLevel
// (20.1.2.6, 20.1.2.7, 20.1.2.16, 20.1.2.19, 20.1.2.20, 20.1.2.22). They live
// beside the storage rules above rather than with the rest of the namespace,
// because deciding WHERE a receiver keeps its level is the whole content of the
// operation and belongs next to every reader of that place.
uint64_t rtObjectFreeze(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);
uint64_t rtObjectIsFrozen(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);
uint64_t rtObjectSeal(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);
uint64_t rtObjectIsSealed(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);
uint64_t rtObjectPreventExtensions(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);
uint64_t rtObjectIsExtensible(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);

}  // namespace bronze::runtime
