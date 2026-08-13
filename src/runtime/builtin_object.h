#pragma once

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/value.h"

// The seam between the two halves of the `Object` namespace.
//
// builtin_object.cpp owns the members that ask about an object AS A WHOLE — its
// keys, its prototype, its identity, the four copies — and
// builtin_object_descriptor.cpp owns the four whose subject is one PROPERTY
// DESCRIPTOR reified as an object: `getOwnPropertyDescriptor`,
// `getOwnPropertyDescriptors`, `defineProperty`, `defineProperties`. That is
// one subject rather than a line count: 6.2.6.4 FromPropertyDescriptor and
// 6.2.6.5 ToPropertyDescriptor are a round trip between bronze's internal form
// — a shape slot, a dictionary entry's `writable` and `configurable` bits — and
// the object a program can hold, and every one of the four is on one side of it
// or the other.
//
// What crosses the seam is declared here and nowhere else: the receiver
// classification both halves need (a member whose step 1 is ToObject wants
// somewhere to read own keys FROM, and one whose step 1 is "if O is not an
// Object, throw" wants a property TABLE), and the two members each half is
// defined in terms of one of the other's.

namespace bronze::runtime {

// Where a receiver's OWN KEYS come from — which is what the members whose step
// 1 is ToObject actually need, and it is less than the object itself.
//
// The box is deliberately not built, the arrangement `Object.getPrototypeOf`
// takes for a primitive. Here it buys more than a skipped allocation: a Number
// object and a Symbol object are boxes bronze cannot make at all, having no
// `Number.prototype` or `Symbol.prototype` to point them at — and neither has
// an own property, so `None` is the COMPLETE answer rather than the one bronze
// can reach. `rtObjectRequirePropertyTable` below stays the gate for
// `defineProperty` and `defineProperties`, whose step 1 is "If O is not an
// Object, throw" (20.1.2.4, 20.1.2.3) and not ToObject.
enum class ObjectOwnKeys {
    Shape,        // a plain object: its own keys are in its shape
    StringChars,  // a primitive string: 10.4.3 synthesises them from the characters
    Namespace,    // a module namespace: 10.4.6.2's sorted export names
    None,         // a number, a boolean, a symbol: the box has no own property
    Threw,        // null or undefined: ToObject has no answer, and this raised it
};

ObjectOwnKeys rtObjectOwnKeysOf(Value v, const char* member);

// The receiver of an `Object` member that needs a property TABLE — one it can
// describe, redefine, or copy into. False means a TypeError is pending.
bool rtObjectRequirePropertyTable(Value v, const char* member);

// Does this value keep its properties in a shape? The predicate the two halves
// share, so that "a plain object" means one thing across both files.
bool rtObjectIsPlain(Value v);

// ToPropertyKey (7.1.19) as the text an own-key question compares. A SYMBOL is
// never one of the keys a string or an empty box has, so the caller answers for
// one without converting it — which is also the only way to answer, since
// ToString of a symbol is a TypeError. ALLOCATES.
std::string rtObjectKeyTextOf(Value keyVal);

// The two members each half is defined in terms of one of the other's.
// `getOwnPropertyDescriptors` is a loop over the keys `getOwnPropertyNames`
// collects (20.1.2.9 step 2), and `Object.create`'s second argument is
// `defineProperties`' own loop (20.1.2.2 step 3) — so each calls the other
// rather than growing a second copy that could drift.
uint64_t rtObjectGetOwnPropertyNames(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);
bool rtObjectDefineFromDescriptors(Rooted<Value>& target, Rooted<Value>& descriptors);

// The four descriptor members themselves, in the shape the namespace table
// installs them in.
uint64_t rtObjectDefineProperty(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);
uint64_t rtObjectDefineProperties(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);
uint64_t rtObjectGetOwnPropertyDescriptor(uint64_t, uint64_t, uint32_t argc,
                                          const uint64_t* argv);
uint64_t rtObjectGetOwnPropertyDescriptors(uint64_t, uint64_t, uint32_t argc,
                                           const uint64_t* argv);

}  // namespace bronze::runtime
