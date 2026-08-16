#pragma once

#include <cstdint>
#include <string>

#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/value.h"

// The Promise core (ECMA-262 27.2): states, reactions, the resolve functions
// and thenable adoption. Split from builtin_promise.cpp — which owns the
// constructor, the prototype's methods and the statics — the way the spec
// splits 27.2.1 (abstract operations) from 27.2.4/27.2.5 (the API): the async
// driver (builtin_async.cpp) needs everything HERE and nothing THERE.
//
// A CAPABILITY is the record 27.2.1.5 names, and it has two forms for one
// reason: over %Promise% the resolving functions are never materialized (the
// promise's own latch is exactly what they would consume), while over a
// SPECIES they are the pair the constructor handed the executor and calling
// them is the only way a subclass that wraps resolve can be observed. The slot
// layout and both forms are `CapabilitySlot` below.

namespace bronze::runtime {

// A promise's internal slots (27.2.6). [[PromiseState]] and [[PromiseResult]]
// under the spec's names; the two reaction lists hold (handler, capability)
// pairs FLATTENED into one array each — two Values per reaction — because a
// reaction record has exactly two fields bronze keeps and a wrapper object
// per reaction would be allocation for no reader. `IsHandled` is
// [[PromiseIsHandled]]. `AlreadyResolved` is the [[AlreadyResolved]] latch of
// the promise's FIRST resolving-function pair — the executor's, and every
// runtime-internal settle — kept on the promise itself because that pair's
// env IS the promise. A thenable job's pair carries a latch of its own
// (27.2.1.3 gives every pair a fresh one), in the pair's env object.
namespace PromiseSlot {
enum : uint32_t {
    State,
    Result,
    FulfillReactions,
    RejectReactions,
    IsHandled,
    AlreadyResolved,
    kCount,
};
}

// [[PromiseState]]'s three values, stored as a double in the State slot.
namespace PromiseState {
enum : uint32_t { Pending, Fulfilled, Rejected };
}

// ---- builtin_promise.cpp: the intrinsic objects -----------------------------

// `Promise.prototype` and the constructor, built together on first use (they
// hold each other, like the Error classes). The instance shape is the one
// root shape every promise is created with; sharing it is what keeps the
// brand check and the inline caches honest.
Value rtPromisePrototype();
class Shape* rtPromiseInstanceShape();

// `Promise` by the name lowering resolved; `undefined` for anything else.
Value rtPromiseConstructor(const std::string& name);
// Is this function object THE Promise constructor — for the property path's
// static-member hooks and for `extends`' refusal.
bool rtIsPromiseConstructor(Value fn);
// A static 27.2.4 defines and bronze has not built (`withResolvers`, `try`),
// diagnosed by name rather than read as `undefined`.
void rtCheckPromiseStaticMember(const std::string& key);

// ---- promise.cpp: the core machinery ----------------------------------------

// The brand: allocated as a promise, not forged over the prototype. Both
// halves matter — see rtIsIteratorObject for the precedent.
bool rtIsPromise(Value v);

// A fresh pending intrinsic promise. ALLOCATES.
Value rtNewPromise();

// The same, with the [[Prototype]] 10.1.13 takes from NewTarget rather than
// %Promise.prototype% — the one thing `new (class extends Promise)()` needs
// that `new Promise()` does not. `shape` is the constructor's memoized instance
// shape, so a subclass costs no shape per promise. ALLOCATES.
Value rtNewPromiseWithShape(class Shape* shape);

// The BRAND, and not `rtIsPromise`: an object allocated as a promise, whatever
// its prototype is. A subclass instance is one and its [[Prototype]] is the
// subclass's, so the identity check `rtIsPromise` makes — "the prototype is
// %Promise.prototype%" — answers false for it while every one of 27.2.5's
// methods must still work on it. The two are separate because the ONE place
// that wants identity rather than the brand is 27.2.4.7's single-tick rule.
bool rtIsPromiseObject(Value v);

// The resolve/reject half of a resolving-function PAIR whose latch is the
// promise's own (27.2.1.3, for the pair whose env is the promise): checks and
// sets [[AlreadyResolved]], then runs the resolution steps. Every settle that
// originates OUTSIDE the promise's own machinery comes through these two —
// the executor's functions, the combinators, the async driver — which is what
// makes "first settle wins" hold across all of them.
void rtResolvePromise(Rooted<Value>& promise, Rooted<Value>& value);
void rtRejectPromise(Rooted<Value>& promise, Rooted<Value>& reason);

// 27.2.4.7 PromiseResolve over the intrinsic. THE single-tick rule lives
// here: an argument that already is an intrinsic promise is returned
// UNCHANGED (step 2), so awaiting one subscribes it directly — one reaction
// job, no wrapper promise, no extra tick. ALLOCATES on the other arm.
Value rtPromiseResolveValue(Rooted<Value>& v);

// A PromiseCapability Record (27.2.1.1): the promise, and the two resolving
// functions the constructor handed the executor. An object with internal slots
// so that nothing a program can reach ever sees it.
//
// For %Promise% the two function slots stay EMPTY and `rtSettleCapability`
// settles the promise through its own latch — which is exactly what those
// functions would do, so building them per `then` would buy nothing observable.
// They are filled only when the capability was constructed over a SPECIES, and
// there the difference is real: a subclass whose constructor wraps the executor's
// resolve is observable only if the wrapper is what gets called.
namespace CapabilitySlot {
enum : uint32_t { Promise, Resolve, Reject, kCount };
}

// 27.2.1.5 NewPromiseCapability(C). `ctor` undefined or %Promise% takes the
// intrinsic path; anything else is constructed with a capturing executor and
// checked. Returns `undefined` with an exception pending on every failure
// path. ALLOCATES, and may run USER CODE (the constructor).
Value rtNewPromiseCapability(Rooted<Value>& ctor);
Value rtNewPromiseCapabilityForIntrinsic();

// The promise half — what `then` returns.
Value rtCapabilityPromise(Value capability);

// Resolve or reject through the capability: the captured function when there is
// one, the promise's own latch otherwise. A non-object capability is the
// no-capability reaction (the async driver's) and settles nothing.
void rtSettleCapability(Rooted<Value>& capability, Rooted<Value>& value, bool reject);

// 27.2.4.7.1 PromiseResolve(C, x) — the general form of `rtPromiseResolveValue`
// above, which is its C = %Promise% arm. `ctor` undefined or %Promise% takes
// that arm unchanged; anything else builds a capability over the constructor,
// so `MyPromise.resolve(1)` is a MyPromise. May run USER CODE.
Value rtPromiseResolveWith(Rooted<Value>& ctor, Rooted<Value>& x);

// 7.3.20 SpeciesConstructor(O, %Promise%): O's `constructor`, then its
// @@species, defaulting to %Promise% at each step. `undefined` — meaning "the
// intrinsic, take the fast path" — is the answer for every promise a program
// did not subclass, decided by one prototype compare before any property is
// read. May run USER CODE (a `constructor` getter).
Value rtPromiseSpeciesConstructor(Rooted<Value>& promise);

// 27.2.5.4.1 PerformPromiseThen. Handlers that are not callable are treated
// as absent (the identity/thrower defaults). `capability` undefined means the
// reaction settles nothing — the await path. Marks the promise handled and
// unparks it, whatever the handlers are.
void rtPerformPromiseThen(Rooted<Value>& promise, Rooted<Value>& onFulfilled,
                          Rooted<Value>& onRejected, Rooted<Value>& capability);

// A native closure over `env`, which the promise machinery and the async
// driver between them build about twenty of. Its own function because
// `FunctionHeader::create` leaves two things to the caller and forgetting
// either is silent rather than loud:
//
//   - `header.flags` starts at a raw 0, which for a Tag::Object allocation
//     reads as HeapKind::Plain. A FunctionHeader wearing that flag is not
//     merely uncallable — the collector and the property path would walk its
//     `code` pointer as an `ObjectHeader`'s shape word.
//   - `create` ALLOCATES, so the environment must be read back THROUGH ITS
//     ROOT afterwards; a copy passed as the argument names a pre-collection
//     address (the bug bound functions and the embed API each record above
//     their own copy of this sequence).
Value rtMakeNativeClosure(NativeFunctionCode code, Rooted<Value>& env, uint32_t arity);

// The executor's resolving functions as function OBJECTS (the one place the
// pair is program-visible). Their env record is the promise itself.
Value rtMakeResolvingFunction(Rooted<Value>& promise, bool isReject);

// The two job bodies, called by the drain (microtask.cpp) and nothing else.
void rtRunReactionJob(Rooted<Value>& handler, Rooted<Value>& capability,
                      Rooted<Value>& argument, bool rejected);
void rtRunThenableJob(Rooted<Value>& promise, Rooted<Value>& thenable, Rooted<Value>& thenFn);

// The text an unhandled rejection is reported with: the rejection reason as
// `Name: message` for an Error instance, else in console.log's rendering.
std::string rtPromiseRejectionText(Value promise);

// State readers, for the unit tests and for console.log if it learns to
// print a promise. A non-promise is a caller error.
uint32_t rtPromiseStateOf(Value promise);
Value rtPromiseResultOf(Value promise);

}  // namespace bronze::runtime
