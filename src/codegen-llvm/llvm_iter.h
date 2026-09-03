#pragma once

// The for-of step, inlined.
//
// The bill: on `many_meshes` (360 frames), `bronze_iter_step` is 5.41 M helper
// entries and `bronze_iter_value` another 3.60 M — 12.7 % of the whole dynamic
// helper bill, for a loop body that in the common case does nothing but read
// `arr[i]` and add one to `i`. Neither call allocates and neither runs user
// code; they are call overhead over three loads and two stores.
//
// Why an inline path is SOUND here, stated once. `iter.open` classifies the
// value it was handed exactly once (runtime/iterator.cpp, `rtOpenIterator`) and
// writes the answer into the record's `kind` word. Everything a user program
// can do to change how a value iterates — a `[Symbol.iterator]` of its own, one
// patched onto a prototype, a generator — is a decision the OPEN makes, and a
// record it classified as an array walk is one the runtime itself would step by
// cursor. So this path re-derives nothing: it reads the open's answer, refuses
// every value but the one kind it can walk, and calls `bronze_iter_step` for
// the rest. Byte for byte it emits what `stepFast`'s `Array` arm does, which is
// why the two cannot answer differently — including the one thing about it that
// looks like a bug and is 23.1.5.1: a HOLE iterates as `undefined` rather than
// being skipped, because the spec's `next` reads with Get.
//
// Deliberately ARRAY ONLY. A typed array's step has to ask whether the buffer
// was detached or resized out from under the walk (a TypeError, not a quiet
// `done`) and has to allocate for a BigInt element — two things this path is
// built not to do — and three.js's hot for-of walks plain arrays. A typed-array
// record simply takes the helper, at exactly the cost it paid before.
//
// Seam: BRONZE_NO_ITER_FAST=1, read from the per-thread ABI block. It is the
// one seam in this chunk that generated code loads, because it is the one
// mechanism that lives in generated code.

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Emits `iter.open` and returns the i64 record Value. An ARRAY source builds
// its record inline — the classification `rtOpenIterator` would make for it is
// its header flags and nothing else, and the record is six stores into the
// inline-allocation window (llvm_construct.cpp's bump pattern) — so the
// per-loop record allocation stops being a helper call. Every other source,
// and a window without headroom, takes bronze_iter_open at exactly the old
// cost. Same seam as the step: BRONZE_NO_ITER_FAST=1.
llvm::Value* emitIterOpen(llvm::IRBuilder<>& builder, const AbiFns& abi,
                          const AbiGlobals& globals, llvm::Value* srcBits);

// Emits `iter.step` and returns its i1 result: true when the record advanced
// and `current` now holds the element, false when the walk is finished (or the
// helper decided so). The Array kind's END is inline too: done := true,
// current := undefined, answer false — the helper's exact foot.
llvm::Value* emitIterStep(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* recBits);

// Emits `iter.value` and returns the i64 (NaN-boxed) element the last step
// produced. A load of one word, guarded by the record's own kind tag — the
// helper's `fatal()` on a non-record is an internal invariant, and this path
// falls back to it rather than restating it.
llvm::Value* emitIterValue(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* recBits);

// Emits `iter.close`. A record whose kind is below `Protocol` — an array, a
// string, a typed array, a Map or Set walked by cursor — has nothing to close
// (runtime/iterator.cpp's `bronze_iter_close` returns on that test before it
// touches anything), and every array destructuring pays one of these, so the
// test is made inline and the helper is called only for a record that holds an
// iterator object. Same seam as the step.
void emitIterClose(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* recBits,
                   bool suppress);

}  // namespace bronze::codegen_llvm
