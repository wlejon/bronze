#pragma once

#include <cstdint>

#include "runtime/gc.h"
#include "runtime/string.h"
#include "runtime/value.h"

// Subclassing a native constructor (ECMA-262 10.1.13 OrdinaryCreateFromConstructor
// and 10.1.14 GetPrototypeFromConstructor), in one place.
//
// THE PROBLEM. `class MyMap extends Map {}` has to produce an object with
// [[MapData]] on it. In the specification the BASE constructor allocates:
// `Map` runs OrdinaryCreateFromConstructor(NewTarget, "%Map.prototype%",
// « [[MapData]] »), so the object is a Map, and its [[Prototype]] comes from
// NewTarget rather than from `Map` — which is what puts `MyMap.prototype` on
// the chain. The derived constructor's `this` IS that object, bound when
// `super()` returns, and the class fields initialize on it afterwards.
//
// THE MECHANISM. bronze allocates the instance ONCE, in `bronze_construct`,
// before any constructor body runs — and the function being constructed there
// is exactly NewTarget. So the only thing missing is for that one allocation
// site to know which exotic object to build, and the answer is a byte on the
// constructor (`FunctionHeader::native_base`) that `bronze_class_extends`
// copies down the chain as each `extends` link is made. `new MyMap()` therefore
// allocates a real MapHeader whose [[Prototype]] is `MyMap.prototype`, hands it
// to the derived constructor as `this`, and `super()` — which is `Map`'s body
// run on that receiver — fills the entries in place instead of building a
// collection of its own and returning it.
//
// WHERE THE PROTOTYPE LIVES. A Map, a Set and an Array carry no shape, so there
// is no slot on them for a [[Prototype]] and their members are answered from a
// table beside the value. What they DO have is the side object holding their
// ordinary named properties (`MapHeader::properties`, `ArrayHeader::properties`)
// — the ordinary-object half of an exotic object — and a subclass instance's
// [[Prototype]] is that box's. The read path walks it before the builtin table,
// so a subclass method shadows a builtin one exactly as the chain says, and an
// instance that is NOT a subclass has no box at all, which is what keeps the
// plain-array path free of every check this adds.
//
// A Promise is not in that group: 27.2 gives it internal SLOTS on an ordinary
// object, so bronze already builds it with a shape and the [[Prototype]] is the
// shape's, needing nothing from here beyond picking NewTarget's shape.

namespace bronze::runtime {

// The exotic objects a `new` can be required to allocate. `None` is every
// ordinary constructor and is deliberately zero, so a FunctionHeader that was
// merely zeroed says "ordinary".
namespace NativeBase {
enum : uint8_t {
    None = 0,
    Array,
    Map,
    Set,
    Promise,
    Count,
};
}

// Which of the above a `new fn(...)` must allocate. Answers for an intrinsic by
// identity and for a class by the byte `extends` recorded on it, so the two
// cases a construction cares about — `new Map()` and `new (class extends Map)()`
// — arrive at the same allocation.
//
// MAY ALLOCATE. The identity half compares against `Array`, `Promise`, `Map`
// and `Set`, and the FIRST read of any of them builds it — so the caller must
// already hold `fn` in a root, or the answer is about an address the collector
// has just moved. The recorded-byte half, which is every class, returns before
// any of that.
uint8_t rtNativeBaseOf(Value fn);

// Record that `derived` inherits `base`'s allocation. Called by
// `bronze_class_extends` once per link, which is why nothing walks a chain.
// Both arrive rooted because reading `base`'s answer can build an intrinsic.
void rtInheritNativeBase(Rooted<Value>& derived, Rooted<Value>& base);

// Make a native constructor's STATICS reachable through `extends`.
//
// 15.7.14 step 6 gives a derived constructor the base constructor as its
// [[Prototype]], so `MyArr.of` is `Array.of` found by an ordinary walk — and
// bronze already builds that walk, between the `properties` boxes the two
// constructors keep their own properties in. What breaks it is that several
// intrinsics answer their statics BESIDE the value, off a table keyed on the
// constructor's code pointer (`Array.of`, `Array.from`, `Array.isArray`,
// `Map.groupBy`), because an interned function singleton had no box worth
// building. A subclass is a different function object with a different code
// pointer, so the table never fires for it and the walk finds nothing: a
// silent `undefined` where the language names a function.
//
// So the statics are REALIZED — written into the base's box as ordinary
// non-enumerable own properties — at the one moment it starts to matter, which
// is the `extends` link. `Promise` needed none of this because its statics
// were already real properties, and that is the arrangement this makes
// uniform rather than a second mechanism beside it. Idempotent, and never
// reached by a program that subclasses nothing.
//
// The function objects are the interned ones the tables hand out, so
// `MyArr.of === Array.of` — one function, two ways of reaching it.
void rtRealizeNativeStatics(Rooted<Value>& ctor);

// The instance, built with the [[Prototype]] 10.1.14 derives from `ctor` —
// which is NewTarget at every call site. ALLOCATES, so `ctor` is a root and
// the result must be rooted by the caller before anything else runs.
Value rtAllocateNativeBaseInstance(uint8_t kind, Rooted<Value>& ctor);

// The exotic object the construction CURRENTLY RUNNING allocated, held for as
// long as its constructor body — and every derived body above it — is on the
// stack. `bronze_construct` pushes one; a native constructor body asks whether
// its receiver IS it.
//
// This is what tells `Map`'s body "the object you must fill is the one you were
// handed" apart from "a program called `Map.call(someMap)`", which 24.1.1.1
// step 1 makes a TypeError. An identity compare against a thread-local answers
// it exactly, and deliberately NOT `NewTargetScope`: the inline dynamic-call
// path in generated code skips that scope's push whenever the module reads no
// `new.target`, so `bronze_get_new_target` is the only observer it is sound
// for, and a second observer would read a stale target on that path.
struct NativeReceiverScope {
    Rooted<Value> receiver_;
    NativeReceiverScope* prev_{nullptr};
    explicit NativeReceiverScope(Value receiver);
    ~NativeReceiverScope();
};

// Is `v` the object the running construction allocated? False for undefined,
// so a plain call — which has no receiver at all — never matches.
bool rtIsNativeConstructReceiver(Value v);

// `class D extends <native>` for a native whose instances bronze cannot
// produce through the mechanism above: refused BY NAME, because the alternative
// is a derived constructor handing back a plain object that answers `undefined`
// to every method of the thing it claims to be. Returns normally when `base` is
// subclassable (a native this file handles, or any ordinary function).
//
// Rooted, because the refusal list is a run of identity probes and each one may
// BUILD the intrinsic it compares against.
void rtCheckNativeBaseExtends(Rooted<Value>& base);

// The ordinary-object half of an exotic instance: the box holding its own named
// properties, whose [[Prototype]] is the instance's. `undefined` for an
// instance that has neither — which is every array and every collection a
// program did not subclass and did not write an expando on, and is the whole
// fast path.
Value rtExoticPropertyBox(Value obj);

// The [[Prototype]] a subclass instance of an exotic kind carries, or
// `undefined`. `Object.getPrototypeOf` and the property path both ask.
Value rtExoticSubclassPrototype(Value obj);

// Read `key` off the ordinary half — own properties first, then the chain above
// them. True when the name was found anywhere on it, which is the only answer
// separating "the subclass defines this" from "the builtin table is the
// answer"; a stored `undefined` is a real property and must not fall through.
// `recv` is the receiver an accessor found on the chain is run against, so a
// getter on a subclass prototype sees the collection and not the box.
bool rtExoticNamedRead(Rooted<Value>& recv, StringHeader* keyHeader, Value& out);

}  // namespace bronze::runtime
