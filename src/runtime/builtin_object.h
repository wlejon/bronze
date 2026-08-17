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
// takes for a primitive. It buys more than a skipped allocation: a Number
// object, a Boolean object and a Symbol object have no own property at all, so
// `None` is the COMPLETE answer rather than the one bronze can reach — and for
// a symbol it is also the ONLY one, since 20.4.3 gives `Symbol.prototype` no
// [[SymbolData]] slot and bronze allocates no object that carries one.
// `rtObjectRequirePropertyTable` below stays the gate for
// `defineProperty` and `defineProperties`, whose step 1 is "If O is not an
// Object, throw" (20.1.2.4, 20.1.2.3) and not ToObject.
enum class ObjectOwnKeys {
    Shape,        // a plain object: its own keys are in its shape
    StringChars,  // a primitive string: 10.4.3 synthesises them from the characters
    Namespace,    // a module namespace: 10.4.6.2's sorted export names
    Function,     // a function: its statics are in FunctionHeader::properties
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
uint64_t objectGetPrototypeOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);
uint64_t objectSetPrototypeOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);

// Does `proto` name the prototype `obj` already has, for a receiver kind that
// carries no shape? The one question `Object.setPrototypeOf` and the
// `__proto__` setter must answer the same way, so that 10.1.2 step 2's
// non-storing success is one rule and not two.
bool rtSamePrototypeAsCurrent(Value obj, Value proto);

// The [[Prototype]] of an array or a function — the two kinds whose prototype
// is fixed by the specification and therefore needs no storage. False for
// anything else.
bool rtShapelessPrototypeOf(Value obj, Value& out);

// What an own-property test found BESIDES "it is there": the attributes 6.2.6
// gives the property, and the values behind it.
//
// `writable` and `configurable` default TRUE because that is what a receiver
// kind whose storage cannot express either actually has: every kind that keeps
// no integrity level (integrity.h names them — a Map, a Set, a typed array, an
// ArrayBuffer, a RegExp) has no route to non-configurable at all, so the
// default is the fact and not a guess. They are also the only two fields
// 10.5's invariant checks gate on, so the permissive default can only ever
// fail to fire a check, never fire a wrong one.
//
// `enumerable` defaults FALSE and is filled positively, which is the
// convention the switch was written against: the non-enumerable own properties
// (an array's `length`, a function's `name`) are the ones that say nothing.
struct OwnPropertyDetail {
    bool accessor{false};
    bool writable{true};
    bool enumerable{false};
    bool configurable{true};
    // The data property's value, or the accessor's pair. Read off a slot
    // without allocating, so a caller that goes on to allocate must root them
    // first.
    //
    // `valueKnown` is false where producing the value would ALLOCATE, and this
    // question is answered without allocating on purpose — a string exotic
    // object's index, a function's `name`, an array's named property. The one
    // check that reads `value` (10.5.8's, against a non-writable
    // non-configurable data property) skips itself when the value is unknown,
    // so the flag can only cost a check that does not fire.
    bool valueKnown{false};
    Value value{Value::fromUndefined()};
    Value getter{Value::fromUndefined()};
    Value setter{Value::fromUndefined()};
};

// 10.1.5-shaped [[GetOwnProperty]]: does this receiver have an own property
// under `keyVal`, and with what attributes? One implementation, in
// builtin_object_proto.cpp, so that `hasOwnProperty`, `propertyIsEnumerable`,
// a proxy's forwarded descriptor read and 10.5's invariant checks cannot drift
// about what a receiver owns.
bool rtOwnPropertyOf(Rooted<Value>& self, Value keyVal, OwnPropertyDetail& out);

// The same question asked by the two callers that want only the enumerability
// — `hasOwnProperty` and `propertyIsEnumerable`. A thin forward, so the narrow
// spelling cannot become a second implementation.
bool rtOwnPropertyOf(Rooted<Value>& self, Value keyVal, bool& enumerable);

}  // namespace bronze::runtime
