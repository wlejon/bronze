#pragma once

// A realm's additions to the frozen intrinsics.
//
// Lockdown freezes every intrinsic (lockdown.h), and old libraries still add
// methods to the builtin prototypes: `Number.prototype.clamp = function ...`
// (a melonJS game), `Object.prototype.clone = ...` (a Pac-Man clone), a
// polyfill's `next` on an iterator prototype. On the web each succeeds and
// every value of that kind has the method; here the write was refused. The
// intrinsics are shared by every realm on the thread, so the addition cannot
// go on the object: it goes to the realm that made it.
//
// A WRITE of a key a hardened intrinsic does not have, by assignment or by
// defining a data property on it, is kept in the current realm's table
// (Realm::intrinsicAdditions) and succeeds; a key the intrinsic has stays
// frozen. A READ whose walk up the prototype chain found the key nowhere asks
// the table, for each hardened link in chain order, before answering
// undefined. That is exactly where the property would have been found: an
// intrinsic's prototype is always another intrinsic, so no ordinary object
// sits below one to shadow it. Each addition moves the prototype-mutation
// epoch, so no read site keeps a cached miss of the key.
//
// What an addition is not: an own property of the intrinsic.
// `hasOwnProperty`, `Object.keys` and for-in do not see it; a read, a call
// and `in` (HasProperty, which `indexOf` over an array's holes asks too) do. An addition is a data property, writable, enumerable and configurable,
// as an assignment makes one; defining an accessor on an intrinsic is still
// refused, and `delete` removes one. A program that freezes, seals or
// prevents extensions of an intrinsic itself closes it in its realm: what it
// added stays, and a new key is refused from then on, as the spec has it for
// an object the program froze. A symbol-keyed addition to Array.prototype or RegExp.prototype is
// not read from an array or a RegExp, whose symbol-keyed reads are answered
// by rt_prop_symbol.cpp; one to Object.prototype or a prototype object that
// is walked as one is.

#include "runtime/gc.h"
#include "runtime/property_key.h"
#include "runtime/value.h"

namespace bronze {

struct ObjectHeader;

namespace runtime {

// `obj` refused to add `key` because it is not extensible: when it is a
// hardened intrinsic, keep `value` as the realm's addition instead and answer
// true (the write succeeded).
bool rtIntrinsicAdditionSet(ObjectHeader* obj, PropertyKey key, Value value);

// A read from `start` found `key` nowhere on its chain: the realm's addition
// to the first hardened link that has one, if any.
bool rtIntrinsicAdditionGet(const ObjectHeader* start, PropertyKey key, Value& out);

// The realm's addition of `key` to `holder` alone, for the read ladders
// that answer an intrinsic's members from a table rather than by walking it
// (an array's Array.prototype, a function's statics and Function.prototype).
bool rtIntrinsicAdditionAt(const ObjectHeader* holder, PropertyKey key, Value& out);

// Whether the current realm has made any addition: the one test the paths
// that ask for one pay when none was made.
bool rtIntrinsicAdditionsAny();

// HasProperty's last step, after the walk found `key` nowhere: whether an
// addition on `obj` or on its [[GetPrototypeOf]] chain names it. The walk
// stops at a proxy, whose `has` trap already answered.
bool rtIntrinsicAdditionHas(Rooted<Value>& obj, PropertyKey key);

// Whether the realm added an index-like key to `obj`, for the array-hole
// scan of the prototype chain. Allocates nothing.
bool rtIntrinsicAdditionHoldsIndex(const ObjectHeader* obj);

// `delete` of a key `obj` does not have: removes the realm's addition of it.
void rtIntrinsicAdditionDelete(ObjectHeader* obj, PropertyKey key);

// The program froze, sealed or prevented extensions of `obj` itself
// (Object.freeze, Reflect.preventExtensions): when it is a hardened
// intrinsic, the realm takes no more additions to it, and a new key is
// refused as on any frozen object.
void rtIntrinsicAdditionsClose(Value obj);

}  // namespace runtime
}  // namespace bronze
