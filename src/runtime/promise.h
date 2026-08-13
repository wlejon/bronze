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
// There is no NewPromiseCapability over an arbitrary constructor and no
// @@species anywhere in this file, deliberately: bronze's promises are the
// intrinsic and only the intrinsic. Subclassing Promise is refused by name at
// `extends` (rt_object.cpp), so a capability is always "a fresh intrinsic
// promise plus its two resolving functions" — which is `rtNewPromise` and the
// internal settle functions, with the function objects never materialized.

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
