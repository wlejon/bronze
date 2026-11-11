#pragma once

#include "il/il.h"
#include "lower/typed_loop_version.h"

namespace bronze::lower {

// TYPED-BODY VERSIONING: an IL -> IL pass that gives a loop-free function
// body whose element reads and writes go through views it was HANDED a second
// copy, entered behind one `is.typed_array r, kTypedArrayWordView` test per
// such receiver, in which those accesses are flagged
// kElemFlagWordViewReceiver (il_pin.h).
//
// What it is for is three.js's flat-array kernels — Quaternion.slerpFlat,
// multiplyQuaternionsFlat, the PropertyMixer mixers — called once per bone
// per frame with a Float32Array or Float64Array the compiler cannot see the
// kind of: `slerpFlat(dst, dstOffset, src0, srcOffset0, src1, srcOffset1, t)`
// reads its two sources through the dynamic element path, each read a tag
// test, an Array header compare it fails, the view compare, a ladder over the
// element kinds and a box, and writes `dst` the same way. And pixi.js's
// batcher, `packQuadAttributes(element, float32View, uint32View, index, ..)`,
// 24 stores a sprite into a Float32Array and a Uint32Array over one buffer.
// A receiver is any WORD VIEW (Int32Array, Uint32Array, Float32Array,
// Float64Array): which of the four kinds it is the copy still asks at each
// access, two compares against the view's kind word that the accesses to one
// view share, so ONE copy serves every mix of kinds a call hands it
// (slerpFlat sees a Float64Array destination with Float32Array sources from
// the interpolant, and three Float64Array views from the mixer; the batcher
// a float and an integer view), where a copy per kind would need one per
// combination.
//
// Nothing about the accesses changes but the inline path the backend emits
// for them (il_elem_word_view.cpp): a Number index outside the view reads
// `undefined` and drops a Number store inline, as 10.4.5.4/5 say, and an
// index or a value that is no Number takes the same helper the generic path
// takes, so the copy needs no proof about the indices, the values, or what
// the body calls (a view's kind is fixed when it is made; its length and
// bytes are read at each access).
//
// The body is the part of the function dominated by its first real block
// (a guarded region's fast copy included, its slow copy shared by both
// versions), with no loop in it — a loop's views are typed_loop_version's —
// and no handler; its receivers are values defined before it (parameters).
// A body below kMinAccesses accesses, or
// above the size cap, is left alone, as is one whose every access is a
// `fromArray` / `toArray` field copy, which the backend's element runs
// (il_lowering_elem_run.cpp) already fuse behind one guard.
//
// READS. A named read in the body (`uvs.x0` between the batcher's stores) asks
// its receiver's shape and, on a miss, calls the ordinary read and comes back.
// A call that comes back is a place the view may have been detached or moved,
// so every access after it reads the view's length, buffer and bytes over
// again, hit or miss. In the copy the read's guard is asked by a split in
// front of it instead, and a miss leaves for the original body's read: the
// copy then calls nothing between its accesses, and the backend reads each
// view's header once. The claim the guard tests is not known when the copy is
// made (a site is claimed last, lower_final_passes.cpp), so the split holds an
// `is.shape` with no claim and resolveReadExits, run after the claims, gives
// it the claim of the read it stands in front of and makes that read its bare
// load (il.h `shapeGuarded`), or, where the read got no claim, answers it
// `true` and leaves the read as it is. A claim by cell is tested against the
// cell of the original body's read, the one a miss runs: only a site's own
// miss fills its cell, and the copy's read no longer has one.
//
// `cold` receives each original body, by function name, as the loop pass
// gives its original loops: the inliner spends its room on the copy first.
bool applyTypedBodyVersions(il::Module& module, ColdBlocks* cold = nullptr);

// Settles every claimless `is.shape` ("READS" above). Whatever happened to the
// copy since it was made, a test is given a claim only where it still stands
// directly in front of a claimed read of its operand: the test ends its block,
// the block it passes into has no other way in, and nothing before the read
// there can collect or throw. Must run before the backend, which knows no
// claimless test.
void resolveReadExits(il::Module& module);

}  // namespace bronze::lower
