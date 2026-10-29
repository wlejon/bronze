#pragma once

#include "il/il.h"

namespace bronze::lower {

// TYPED-LOOP VERSIONING: an IL -> IL pass that gives an innermost loop over a
// typed view a second copy in which the view's reads and writes are the typed
// element ops, entered behind one `is.typed_array` test per view.
//
// What it is for is a view nothing proved — `geometry.attributes.position
// .array`, read out of an object whose `array` field holds every kind of
// view three.js makes. Each `arr[v]` over such a value is the dynamic element
// path: a tag test, the Array header compare it fails, the view header
// compare, the length, the buffer's data pointer, a compare chain over the
// element kinds, the load and the box. Inside a loop the receiver is the same
// value on every iteration, and the two facts that ladder spends most of its
// instructions establishing — this is a view, and of this kind — are fixed
// when the view is made. So they are tested once, on the loop's entry edge,
// and the copy they lead into holds `elem.get.typed` / `elem.set.typed`.
//
// The kind is all that is fixed whatever the loop calls. A call can detach the
// buffer, resize it, or collect and move it; it cannot change what kind of
// view the value is. The typed ops read the length and the data pointer at
// every access (a detached or out-of-bounds view's length is 0 in its header,
// typed_array.h), exactly as the dynamic path does, so for that much the copy
// needs no proof about the loop's body at all. The data pointer is never
// hoisted, even out of a loop with no call in it: the back edge's interrupt
// poll can suspend the fiber, and a collection there may move the bytes.
//
// A LOOP WITH NOTHING IN IT THAT CAN COLLECT (every instruction left generic
// is `!canCollect`, so it calls nothing that could run script) also reads
// each view's `length` once, on the entry edge, and the copy uses that number
// for its `.length` reads. A number cannot move, and with no script in the
// loop nothing can detach or resize the buffer between the entry and any
// iteration. The entry test then also requires the view's shape to be its
// kind's own and the prototype chain's `length` untouched (the flag
// kTypedArrayPristineLength on `is.typed_array`, the same check the inline
// `length` read makes), so an own `length`, a patched getter, or a
// length-tracking view takes the original loop. With that length in hand an
// index `p` or `p + k` (k a non-negative integral constant), where `p` is a
// header parameter that starts at an integral constant and only steps up by
// integral constants, and the access sits under the loop's `p < length` test,
// is flagged kElemFlagProvenIntegralIndex: the typed op then skips its integer
// test.
//
// A READ becomes typed only where `undefined` and NaN are indistinguishable,
// because the typed read answers NaN for an index outside the view where the
// language answers `undefined`. Its value's every use must be one of: the
// guarded-region pass's `is.number` test of it, which becomes `true`; an
// `unbox.f64` of it, which becomes the typed read's own f64, and whose every
// use applies ToNumber anyway (the arithmetic, the compares, `math.unary`, a
// typed store's value); the value of a store into a view this copy also
// types; or an argument of the trampoline on that same test's false edge,
// which the folded test leaves unreachable. That is the same set of positions
// lower_typed_elem.cpp admits for a PROVEN view, read off the IL instead of
// the AST. A WRITE becomes typed when its index and value are boxed numbers:
// a Number index on a view is always an integer-indexed access (10.4.5), and
// its store is ToNumber, a no-op for a Number, then 23.2.5's store or silent
// skip — never a throw, strict or not.
//
// Two kinds are versioned, Float32 then Float64: what dynamic loops over views
// meet, the vertex and matrix arrays. Any other kind, or a value that is no
// view at all, takes the original loop, unchanged. A loop whose copy would be
// big, which sits inside a handler's region, or which is a guarded region's
// SLOW copy is left alone.
//
// THE SEAM is `BRONZE_NO_TYPED_LOOP_VERSIONS=1`, read once per process: with
// it set the pass changes nothing. `BRONZE_TYPED_LOOP_VERSION_TRACE=1` prints
// one line per loop versioned.
bool applyTypedLoopVersions(il::Module& module);

}  // namespace bronze::lower
