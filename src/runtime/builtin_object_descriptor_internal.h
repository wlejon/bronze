#pragma once

// The seam INSIDE the descriptor subject: what builtin_object_descriptor.cpp
// (6.2.6.5 ToPropertyDescriptor, the decode, and 6.2.6.4 FromPropertyDescriptor,
// the reads) hands to builtin_object_define.cpp (10.1.6.3 and 10.4.2.1, the
// apply, and the two members and the ABI entry point defined over it). Nothing
// here is for a third file: the two halves are one algorithm split at the
// point where reading a program's descriptor object stops and the
// specification's own algorithm over storage begins.

#include "runtime/gc.h"
#include "runtime/value.h"

namespace bronze::runtime::descriptor_internal {

// A descriptor after 6.2.6.5 has run, or as a compile-time literal already
// settled it: which of the six fields the descriptor HAS, and the three
// attribute booleans it wanted. The `value`, `get` and `set` payloads travel
// separately because they are Values and have to be rooted by their caller.
//
// It is the seam between the two halves of 10.1.6.3's one caller. Reading a
// descriptor object is a program the descriptor's author wrote (each field may
// be a getter, and the order is observable); applying the result is the
// specification's own algorithm over storage. Only the first half has anything
// to do with objects, which is what lets a caller that already knows the
// answer — `bronze_define_own_attr` — skip it without a second copy of the
// second half drifting away from this one.
struct DecodedDescriptor {
    bool hasValue = false;
    bool hasWritable = false;
    bool hasEnumerable = false;
    bool hasConfigurable = false;
    bool hasGet = false;
    bool hasSet = false;
    bool wantWritable = false;
    bool wantEnumerable = false;
    bool wantConfigurable = false;
};

// ECMA-262 6.2.6.5 ToPropertyDescriptor on its own: the six reads and the
// three checks, with the payloads left in the roots the caller handed in.
// False with an exception pending on every failure. Separate from the apply
// because 20.1.2.3.1 runs ALL of a batch's decodes before ANY apply, so the
// decoded batch has to be parked between the two halves.
bool decodeDescriptor(Rooted<Value>& desc, DecodedDescriptor& d, Rooted<Value>& value,
                      Rooted<Value>& getter, Rooted<Value>& setter);

// One field of a descriptor object being built, in the order the caller
// writes them — which is 6.2.6.4's order, and `Object.keys(descriptor)`
// prints it.
void putField(Rooted<Value>& obj, const char* name, Rooted<Value>& val);
// The same, for a key that is already a value — a descriptor map's keys are
// the target's own property names, which have no C string to go back to.
void putField(Rooted<Value>& obj, Rooted<Value>& key, Rooted<Value>& val);

// Are the six descriptor fields of a fresh object literal exactly the ones the
// literal WROTE — nothing on `Object.prototype`'s chain answering one of the
// six names? The question `bronze_define_own_attr` asks before it trusts a
// compile-time decode; the answer is memoized on the prototype's shape.
bool literalDescriptorFieldsAreOwnOnly();

}  // namespace bronze::runtime::descriptor_internal
