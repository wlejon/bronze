#pragma once

#include <cstdint>
#include <vector>

#include "il/il.h"

namespace bronze::lower {

// TYPED METHOD ENTRIES: an IL -> IL pass that gives a class method a second
// entry whose source parameters arrive unboxed, and points the direct edge of
// every method-call site that already holds those arguments unboxed at it.
//
// A method compiles to the uniform dynamic convention because it is reached
// through a property: `v.set(x, y, z)` finds `set` on whatever `v` is at run
// time, so nothing about who calls it can be enumerated the way a module
// function's callers can. The guarded direct edge (il.h `directTarget`)
// already makes the common case a direct call — the site's inline cache
// proves, per call, that the receiver resolves the key to the function the
// edge names — but its arguments are boxed on the way in and tested, unboxed
// or re-defaulted on the way through the body, because the body was written
// for any value.
//
// THE ENTRY IS A COPY OF THE FINISHED BODY, entered differently. It is the
// generic function's IL after every other pass, with each typed parameter
// taking an `f64` (or `bool`) and the body reading `box.<type>` of it where it
// read the parameter. So for an argument that IS a Number, the copy computes
// exactly what the generic body computes — it is the generic body, given that
// value — and nothing in it rests on what inference concluded about the
// method's parameters. What makes it smaller is a local fold of that one fact:
// a tag test of a box answers itself, an unbox of a box is its operand, an
// `=== undefined` of a box is false, and the branches and blocks those decided
// are dropped (typed_entry_fold.cpp). An entry the fold shrank by nothing is
// not kept.
//
// THE EDGE IS DECIDED BY ONE PREDICATE, `typedSignatureAt`: a site's
// signature is read off the caller's own SSA values (an argument defined by
// `box.f64 %x` is `%x`, an f64), and a site is rewired exactly when an entry
// for that signature of that target exists. The set of entries built is the
// set of signatures that predicate returned, so no site can be rewired on a
// fact a different test established. The verifier then re-checks the result
// on its own terms (il/verifier_typed_entry.cpp): the callee is a typed entry
// of the site's direct target, and each typed operand has its parameter's
// type.
//
// WHAT EACH FACT RESTS ON:
//   - the target: CHECKED per call, unchanged from the untyped edge. The
//     guard compares the site's cached code pointer and prototype epoch
//     against the GENERIC function's wrapper; a patched prototype, a
//     reassigned or deleted method, an own property shadowing it, a subclass
//     override or a receiver of another class fails it and takes the cache's
//     generic dispatch, which boxes the unboxed operands again.
//   - the arguments' types: PROVEN locally — they are the caller's IL types.
//   - `this`: passed as the receiver the guard checked; the entry treats it
//     exactly as the generic body does.
//   - arity: PROVEN locally. An entry is built for one argument count, the
//     site's, and only that count's sites reach it (the verifier checks
//     `typedArgc`); the parameters past it read `undefined`, which is what the
//     edge passes there. A site passing more arguments than the method
//     declares keeps the generic dispatch, and the entry has no `arguments`
//     object and no rest parameter, so nothing else about arity is observable.
//
// `call`/`apply`/`Reflect.apply`, a method used as a callback, an extracted
// method: none of these is a method-call site with a direct edge, so none of
// them can reach a typed entry. They call the generic function, which is
// unchanged.
//
// Runs LAST, after the guarded regions: the unboxed arguments at a call site
// are, almost always, values a fast copy made unboxed.
//
// THE SEAM is `BRONZE_NO_TYPED_ENTRIES=1`, read once per process: with it set
// no entry is built and the module is what it was. `BRONZE_TYPED_ENTRY_STATS=1`
// prints one line of counters to stderr.

struct TypedEntryStats {
    uint32_t sitesDirect = 0;     // method calls with a direct target
    uint32_t sitesUnboxed = 0;    // ... whose signature names an unboxed argument
    uint32_t sitesTyped = 0;      // ... rewired to a typed entry
    uint32_t entriesBuilt = 0;
    uint32_t entriesNoGain = 0;   // the copy's fold removed nothing
    uint32_t entriesRefused = 0;  // the folded copy failed the dominance check
    uint32_t entriesCapped = 0;   // a signature past the per-method limit
    uint32_t foldsApplied = 0;
};

// A site's typed signature: per caller parameter of the target, the type the
// argument arrives as (`Dynamic`, `F64` or `Bool`), and the value that carries
// it unboxed (`kNoValue` in a `Dynamic` position).
struct TypedSignature {
    std::vector<il::Type> params;
    std::vector<il::ValueId> unboxed;
    // How many arguments the site passes. The parameters past it are
    // `undefined` on the direct edge, and the entry reads them as such: that is
    // what folds a defaulted parameter's test (`order = this._order`).
    size_t argc = 0;
};

// Whether `fn` can have typed entries at all: a plain method body on the
// uniform convention that the direct edge can reach.
bool typedEntryEligible(const il::Function& fn);

// THE predicate. `defs` maps each of the caller's values to the instruction
// defining it (null for parameters and block parameters). True when `call` is
// a method call whose direct target is eligible and takes at least as many
// parameters as it passes, and either an argument is a box of an unboxed
// f64/bool or a parameter is omitted.
bool typedSignatureAt(const il::Module& module, const std::vector<const il::Instruction*>& defs,
                      const il::Instruction& call, TypedSignature& out);

// The local fold the copy's typed parameters license, to a fixpoint. Returns
// how many instructions and branches it decided (dead-code removal is not
// counted: it follows from a fold and is not one).
uint32_t foldTypedEntry(il::Function& fn);

void applyTypedEntries(il::Module& module, TypedEntryStats* stats);

}  // namespace bronze::lower
