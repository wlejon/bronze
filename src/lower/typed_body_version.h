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
// `cold` receives each original body, by function name, as the loop pass
// gives its original loops: the inliner spends its room on the copy first.
bool applyTypedBodyVersions(il::Module& module, ColdBlocks* cold = nullptr);

}  // namespace bronze::lower
