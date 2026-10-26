#pragma once

// Lockdown: freezing every intrinsic the realms of a thread share.
//
// The builtins are per THREAD, not per realm: every realm's `Object` is the one
// `rtResolveBuiltinGlobal` answers (realm.cpp copies the same values onto each
// global object). An embedder that runs mutually distrustful code in several
// realms on one thread — bro's compartments — therefore cannot let any of
// them change what another calls. Lockdown is SES's answer: before the first
// program runs, every object reachable from the intrinsics is frozen, so they
// are shared and immutable at once. Each realm's global object stays its own
// and stays writable; only what the builtins hold is frozen.
//
// What is reached: the builtin global names (realm.h, rtBuiltinGlobalNames)
// and the intrinsics no global name leads to — the iterator prototypes
// (%ArrayIteratorPrototype%, %MapIteratorPrototype%, ..., %IteratorHelperPrototype%,
// %WrapForValidIteratorPrototype%) and %GeneratorFunction.prototype%,
// %AsyncFunction.prototype% and %AsyncGeneratorFunction.prototype% — then,
// transitively, every own property's value, getter and setter and every
// [[Prototype]], through the same reflective operations a program would use
// (`Reflect.ownKeys`, `Object.getOwnPropertyDescriptor`,
// `Object.getPrototypeOf`). Reading through them is the point: an intrinsic
// that answers some members from a table rather than from storage
// (`Object.prototype`, the constructors' statics) materializes them for the
// read, so the freeze that follows covers them.
//
// Each object is frozen with `Object.freeze` and marked hardened
// (Dictionary::hardened), which is what the OVERRIDE MISTAKE fix keys on: an
// assignment to an object that inherits a non-writable property from a
// hardened object creates the receiver's own property instead of failing, so
// `Ctor.prototype.toString = f` and `MyError.prototype.name = 'MyError'` behave
// as they did before the freeze. The hardened object itself refuses every write
// (silently in sloppy code, a TypeError in strict, like any frozen object).
//
// Frozen intrinsics read as fast as unfrozen ones: a frozen dictionary is one an
// inline cache may name (Shape::readCacheable), and a frozen object never bumps
// the prototype-mutation epoch.
//
// Not frozen: anything a host registers (its globals, its classes' prototypes),
// which is per realm (host_globals.h), and %ThrowTypeError%, which bronze makes
// fresh per `arguments` object rather than sharing.
//
// Standalone bronze programs never call it: they run one program per process,
// and node parity (which the oracle pins) leaves intrinsics mutable.

namespace bronze::runtime {

// Harden the calling thread's intrinsics. Idempotent; a thread that has run a
// program first gets whatever that program did to them frozen in place, so an
// embedder calls it before its first script. `Error.stackTraceLimit` freezes at
// its default (10) along with the rest of `Error`.
void rtLockdown();

// Whether rtLockdown has run on the calling thread.
bool rtLockedDown();

}  // namespace bronze::runtime
